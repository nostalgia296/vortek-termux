#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "adrenotools/driver.h"
#include "vk_context.h"
#include "vulkan_wrapper.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_SOCKET_SUBDIR ".vortek"
#define DEFAULT_SOCKET_NAME "V0"
#define ADRENOTOOLS_HOOK_DIR_ENV "VORTEK_ADRENOTOOLS_HOOK_DIR"

VulkanWrapper vulkanWrapper = {0};
bool vortekSerializerCastVkObject = true;

static volatile sig_atomic_t keepRunning = 1;
static int listenFd = -1;

typedef struct CliConfig {
    char socketPath[PATH_MAX];
    char adrenotoolsHookDir[PATH_MAX];
    const char* libvulkanPath;
    VkContextOptions contextOptions;
    bool once;
    bool vkVersionProvided;
} CliConfig;

typedef struct CliClient {
    int clientFd;
    VkContextOptions contextOptions;
} CliClient;

static void handleSignal(int signalNumber) {
    (void)signalNumber;
    keepRunning = 0;
    if (listenFd >= 0) {
        close(listenFd);
        listenFd = -1;
    }
}

static void setDefaultSocketPath(char* path, size_t size) {
    const char* tmpDir = getenv("TMPDIR");
    if (!tmpDir || !tmpDir[0]) tmpDir = "/data/data/com.termux/files/usr/tmp";
    snprintf(path, size, "%s/%s/%s", tmpDir, DEFAULT_SOCKET_SUBDIR, DEFAULT_SOCKET_NAME);
}

static bool pathJoin(char* output, size_t outputSize, const char* dir, const char* name) {
    const char* separator = "/";
    size_t dirLen = strlen(dir);
    if (dirLen > 0 && dir[dirLen - 1] == '/') separator = "";
    int written = snprintf(output, outputSize, "%s%s%s", dir, separator, name);
    return written >= 0 && (size_t)written < outputSize;
}

static bool hasAdrenotoolsHooks(const char* dir) {
    char path[PATH_MAX];
    if (!pathJoin(path, sizeof(path), dir, "libhook_impl.so") || access(path, R_OK) != 0) return false;
    if (!pathJoin(path, sizeof(path), dir, "libmain_hook.so") || access(path, R_OK) != 0) return false;
    return true;
}

static bool copyDirectoryName(const char* path, char* dir, size_t dirSize, bool trailingSlash) {
    const char* slash = path ? strrchr(path, '/') : NULL;
    if (!slash) {
        int written = snprintf(dir, dirSize, trailingSlash ? "./" : ".");
        return written >= 0 && (size_t)written < dirSize;
    }

    size_t len = (size_t)(slash - path);
    if (trailingSlash) len++;
    if (len == 0) len = 1;
    if (len >= dirSize) return false;

    memcpy(dir, path, len);
    dir[len] = '\0';
    return true;
}

static void setDefaultAdrenotoolsHookDir(const char* programName, char* path, size_t size) {
    const char* envHookDir = getenv(ADRENOTOOLS_HOOK_DIR_ENV);
    if (envHookDir && envHookDir[0]) {
        snprintf(path, size, "%s", envHookDir);
        return;
    }

    const char* prefix = getenv("PREFIX");
    if (prefix && prefix[0]) {
        char prefixLibDir[PATH_MAX];
        if (pathJoin(prefixLibDir, sizeof(prefixLibDir), prefix, "lib") && hasAdrenotoolsHooks(prefixLibDir)) {
            snprintf(path, size, "%s", prefixLibDir);
            return;
        }
    }

    char executablePath[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", executablePath, sizeof(executablePath) - 1);
    if (len > 0) {
        executablePath[len] = '\0';
        if (copyDirectoryName(executablePath, path, size, false)) return;
    }

    copyDirectoryName(programName, path, size, false);
}

static void printUsage(const char* programName) {
    printf("Usage: %s [options]\n", programName);
    printf("\n");
    printf("Options:\n");
    printf("  -s, --socket PATH              Unix socket path (default: $TMPDIR/.vortek/V0)\n");
    printf("  -v, --libvulkan PATH           Custom Vulkan driver path loaded through adrenotools\n");
    printf("      --adrenotools-hook-dir DIR Directory containing libhook_impl.so/libmain_hook.so\n");
    printf("      --vk-version VERSION       Exposed Vulkan API version, e.g. 1.3.0 or packed integer\n");
    printf("      --max-device-memory MB     Clamp reported device memory, 0 disables clamp\n");
    printf("      --image-cache-size MB      Texture decoder cache size, 0 disables cache\n");
    printf("      --resource-memory MODE     auto, opaque-fd, dma-buf, ahardwarebuffer\n");
    printf("      --expose-ext LIST          Comma-separated exposed device extensions\n");
    printf("      --once                     Handle one client and exit\n");
    printf("  -h, --help                     Show this help\n");
}

static const char* optionValue(int argc, char** argv, int* index, const char* optionName) {
    if (*index + 1 >= argc) {
        fprintf(stderr, "vortek-cli: missing value for %s\n", optionName);
        return NULL;
    }

    *index += 1;
    return argv[*index];
}

static bool parseLong(const char* value, long minValue, long maxValue, long* out) {
    char* end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < minValue || parsed > maxValue) return false;
    *out = parsed;
    return true;
}

static bool parseVkVersion(const char* value, int* out) {
    unsigned int major = 0;
    unsigned int minor = 0;
    unsigned int patch = 0;
    char trailing = '\0';

    if (strchr(value, '.')) {
        int count = sscanf(value, "%u.%u.%u%c", &major, &minor, &patch, &trailing);
        if (count < 2 || count > 3) return false;
        *out = VK_MAKE_VERSION(major, minor, patch);
        return true;
    }

    long packed = 0;
    if (!parseLong(value, 0, INT_MAX, &packed)) return false;
    *out = (int)packed;
    return true;
}

static bool parseResourceMemoryType(const char* value, ResourceMemoryType* out) {
    if (strcmp(value, "auto") == 0) {
        *out = RESOURCE_MEMORY_TYPE_AUTO;
        return true;
    }
    if (strcmp(value, "opaque-fd") == 0) {
        *out = RESOURCE_MEMORY_TYPE_OPAQUE_FD;
        return true;
    }
    if (strcmp(value, "dma-buf") == 0 || strcmp(value, "dmabuf") == 0) {
        *out = RESOURCE_MEMORY_TYPE_DMA_BUF;
        return true;
    }
    if (strcmp(value, "ahardwarebuffer") == 0 || strcmp(value, "ahb") == 0) {
        *out = RESOURCE_MEMORY_TYPE_AHARDWAREBUFFER;
        return true;
    }
    return false;
}

static char* trimInPlace(char* value) {
    while (*value && isspace((unsigned char)*value)) value++;

    char* end = value + strlen(value);
    while (end > value && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';

    return value;
}

static bool addExposedExtension(char*** extensions, int* extensionCount, int* extensionCapacity, const char* extensionName) {
    if (!extensionName || !extensionName[0]) return true;

    if (*extensionCount >= *extensionCapacity) {
        int newCapacity = *extensionCapacity > 0 ? *extensionCapacity * 2 : 8;
        char** newExtensions = realloc(*extensions, newCapacity * sizeof(char*));
        if (!newExtensions) return false;
        *extensions = newExtensions;
        *extensionCapacity = newCapacity;
    }

    (*extensions)[*extensionCount] = strdup(extensionName);
    if (!(*extensions)[*extensionCount]) return false;
    *extensionCount += 1;
    return true;
}

static bool addExposedExtensions(char*** extensions, int* extensionCount, int* extensionCapacity, const char* value) {
    char* copy = strdup(value);
    if (!copy) return false;

    char* savePtr = NULL;
    char* token = strtok_r(copy, ",", &savePtr);
    while (token) {
        char* extensionName = trimInPlace(token);
        if (!addExposedExtension(extensions, extensionCount, extensionCapacity, extensionName)) {
            free(copy);
            return false;
        }
        token = strtok_r(NULL, ",", &savePtr);
    }

    free(copy);
    return true;
}

static void freeExposedExtensions(char** extensions, int extensionCount) {
    for (int i = 0; i < extensionCount; i++) free(extensions[i]);
    free(extensions);
}

static bool parseArgs(int argc, char** argv, CliConfig* config, char*** exposedExtensions, int* exposedExtensionCount) {
    int exposedExtensionCapacity = 0;

    setDefaultSocketPath(config->socketPath, sizeof(config->socketPath));
    setDefaultAdrenotoolsHookDir(argv[0], config->adrenotoolsHookDir, sizeof(config->adrenotoolsHookDir));
    config->contextOptions.resourceMemoryType = RESOURCE_MEMORY_TYPE_AUTO;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        const char* value = NULL;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            printUsage(argv[0]);
            exit(0);
        }
        else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--socket") == 0) {
            value = optionValue(argc, argv, &i, arg);
            if (!value) return false;
            snprintf(config->socketPath, sizeof(config->socketPath), "%s", value);
        }
        else if (strncmp(arg, "--socket=", 9) == 0) {
            snprintf(config->socketPath, sizeof(config->socketPath), "%s", arg + 9);
        }
        else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--libvulkan") == 0) {
            value = optionValue(argc, argv, &i, arg);
            if (!value) return false;
            config->libvulkanPath = value;
        }
        else if (strncmp(arg, "--libvulkan=", 12) == 0) {
            config->libvulkanPath = arg + 12;
        }
        else if (strcmp(arg, "--adrenotools-hook-dir") == 0 || strcmp(arg, "--hook-lib-dir") == 0 || strcmp(arg, "--native-library-dir") == 0) {
            value = optionValue(argc, argv, &i, arg);
            if (!value) return false;
            snprintf(config->adrenotoolsHookDir, sizeof(config->adrenotoolsHookDir), "%s", value);
        }
        else if (strncmp(arg, "--adrenotools-hook-dir=", 23) == 0) {
            snprintf(config->adrenotoolsHookDir, sizeof(config->adrenotoolsHookDir), "%s", arg + 23);
        }
        else if (strncmp(arg, "--hook-lib-dir=", 15) == 0) {
            snprintf(config->adrenotoolsHookDir, sizeof(config->adrenotoolsHookDir), "%s", arg + 15);
        }
        else if (strncmp(arg, "--native-library-dir=", 21) == 0) {
            snprintf(config->adrenotoolsHookDir, sizeof(config->adrenotoolsHookDir), "%s", arg + 21);
        }
        else if (strcmp(arg, "--vk-version") == 0) {
            value = optionValue(argc, argv, &i, arg);
            if (!value) return false;
            if (!parseVkVersion(value, &config->contextOptions.vkMaxVersion)) {
                fprintf(stderr, "vortek-cli: invalid Vulkan version: %s\n", value);
                return false;
            }
            config->vkVersionProvided = true;
        }
        else if (strncmp(arg, "--vk-version=", 13) == 0) {
            value = arg + 13;
            if (!parseVkVersion(value, &config->contextOptions.vkMaxVersion)) {
                fprintf(stderr, "vortek-cli: invalid Vulkan version: %s\n", value);
                return false;
            }
            config->vkVersionProvided = true;
        }
        else if (strcmp(arg, "--max-device-memory") == 0) {
            long parsed = 0;
            value = optionValue(argc, argv, &i, arg);
            if (!value || !parseLong(value, 0, SHRT_MAX, &parsed)) {
                fprintf(stderr, "vortek-cli: invalid max device memory\n");
                return false;
            }
            config->contextOptions.maxDeviceMemory = (short)parsed;
        }
        else if (strncmp(arg, "--max-device-memory=", 20) == 0) {
            long parsed = 0;
            value = arg + 20;
            if (!parseLong(value, 0, SHRT_MAX, &parsed)) {
                fprintf(stderr, "vortek-cli: invalid max device memory\n");
                return false;
            }
            config->contextOptions.maxDeviceMemory = (short)parsed;
        }
        else if (strcmp(arg, "--image-cache-size") == 0) {
            long parsed = 0;
            value = optionValue(argc, argv, &i, arg);
            if (!value || !parseLong(value, 0, SHRT_MAX, &parsed)) {
                fprintf(stderr, "vortek-cli: invalid image cache size\n");
                return false;
            }
            config->contextOptions.imageCacheSize = (short)parsed;
        }
        else if (strncmp(arg, "--image-cache-size=", 19) == 0) {
            long parsed = 0;
            value = arg + 19;
            if (!parseLong(value, 0, SHRT_MAX, &parsed)) {
                fprintf(stderr, "vortek-cli: invalid image cache size\n");
                return false;
            }
            config->contextOptions.imageCacheSize = (short)parsed;
        }
        else if (strcmp(arg, "--resource-memory") == 0) {
            value = optionValue(argc, argv, &i, arg);
            if (!value || !parseResourceMemoryType(value, &config->contextOptions.resourceMemoryType)) {
                fprintf(stderr, "vortek-cli: invalid resource memory mode\n");
                return false;
            }
        }
        else if (strncmp(arg, "--resource-memory=", 18) == 0) {
            value = arg + 18;
            if (!parseResourceMemoryType(value, &config->contextOptions.resourceMemoryType)) {
                fprintf(stderr, "vortek-cli: invalid resource memory mode\n");
                return false;
            }
        }
        else if (strcmp(arg, "--expose-ext") == 0) {
            value = optionValue(argc, argv, &i, arg);
            if (!value || !addExposedExtensions(exposedExtensions, exposedExtensionCount, &exposedExtensionCapacity, value)) {
                fprintf(stderr, "vortek-cli: invalid exposed extension list\n");
                return false;
            }
        }
        else if (strncmp(arg, "--expose-ext=", 13) == 0) {
            value = arg + 13;
            if (!addExposedExtensions(exposedExtensions, exposedExtensionCount, &exposedExtensionCapacity, value)) {
                fprintf(stderr, "vortek-cli: invalid exposed extension list\n");
                return false;
            }
        }
        else if (strcmp(arg, "--once") == 0) {
            config->once = true;
        }
        else {
            fprintf(stderr, "vortek-cli: unknown option: %s\n", arg);
            return false;
        }
    }

    config->contextOptions.exposedDeviceExtensions = (const char* const*)*exposedExtensions;
    config->contextOptions.exposedDeviceExtensionCount = *exposedExtensionCount;
    return true;
}

static bool splitLibraryPath(const char* path, char* dir, size_t dirSize, char* name, size_t nameSize) {
    if (!path || !path[0]) return false;

    const char* slash = strrchr(path, '/');
    const char* fileName = slash ? slash + 1 : path;
    if (!fileName[0]) return false;

    if (!copyDirectoryName(path, dir, dirSize, true)) return false;

    int written = snprintf(name, nameSize, "%s", fileName);
    return written >= 0 && (size_t)written < nameSize;
}

static bool mkdirP(const char* dir);

static void* openVulkanLibrary(const CliConfig* config) {
    if (config->libvulkanPath && config->libvulkanPath[0]) {
        char libvulkanDir[PATH_MAX];
        char libvulkanName[PATH_MAX];
        if (!splitLibraryPath(config->libvulkanPath, libvulkanDir, sizeof(libvulkanDir), libvulkanName, sizeof(libvulkanName))) {
            fprintf(stderr, "vortek-cli: invalid custom Vulkan driver path: %s\n", config->libvulkanPath);
            return NULL;
        }

        char tmpDir[PATH_MAX];
        int written = snprintf(tmpDir, sizeof(tmpDir), "%stmp", libvulkanDir);
        if (written < 0 || (size_t)written >= sizeof(tmpDir)) {
            fprintf(stderr, "vortek-cli: custom Vulkan driver path is too long: %s\n", config->libvulkanPath);
            return NULL;
        }

        if (!mkdirP(tmpDir)) {
            fprintf(stderr, "vortek-cli: unable to create adrenotools tmp dir %s: %s\n", tmpDir, strerror(errno));
            return NULL;
        }

        void* libvulkan = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL,
                                                     ADRENOTOOLS_DRIVER_CUSTOM,
                                                     tmpDir,
                                                     config->adrenotoolsHookDir,
                                                     libvulkanDir,
                                                     libvulkanName,
                                                     NULL,
                                                     NULL);
        if (!libvulkan) {
            const char* error = dlerror();
            fprintf(stderr,
                    "vortek-cli: unable to open custom Vulkan driver %s through adrenotools (hook dir: %s): %s\n",
                    config->libvulkanPath,
                    config->adrenotoolsHookDir,
                    error ? error : "unknown error");
        }
        return libvulkan;
    }

    void* libvulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!libvulkan) fprintf(stderr, "vortek-cli: unable to open libvulkan.so: %s\n", dlerror());
    return libvulkan;
}

static bool initVulkan(CliConfig* config) {
    void* libvulkan = openVulkanLibrary(config);
    if (!libvulkan) return false;

    initVulkanWrapper(&vulkanWrapper, libvulkan);
    if (!vulkanWrapper.vkCreateInstance || !vulkanWrapper.vkGetInstanceProcAddr || !vulkanWrapper.vkGetDeviceProcAddr) {
        fprintf(stderr, "vortek-cli: libvulkan is missing required entry points\n");
        return false;
    }

    if (!config->vkVersionProvided) {
        uint32_t apiVersion = VK_API_VERSION_1_0;
        if (vulkanWrapper.vkEnumerateInstanceVersion) {
            VkResult result = vulkanWrapper.vkEnumerateInstanceVersion(&apiVersion);
            if (result != VK_SUCCESS) apiVersion = VK_API_VERSION_1_0;
        }
        config->contextOptions.vkMaxVersion = (int)apiVersion;
    }

    return true;
}

static bool mkdirP(const char* dir) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", dir);

    size_t len = strlen(path);
    if (len == 0) return true;
    if (path[len - 1] == '/') path[len - 1] = '\0';

    for (char* p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(path, 0700) != 0 && errno != EEXIST) return false;
        *p = '/';
    }

    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static bool createSocketDirectory(const char* socketPath) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", socketPath);

    char* slash = strrchr(dir, '/');
    if (!slash) return true;
    if (slash == dir) return true;
    *slash = '\0';

    return mkdirP(dir);
}

static bool removeExistingSocket(const char* socketPath) {
    struct stat st;
    if (lstat(socketPath, &st) != 0) return errno == ENOENT;
    if (!S_ISSOCK(st.st_mode)) {
        fprintf(stderr, "vortek-cli: refusing to remove non-socket path: %s\n", socketPath);
        return false;
    }
    return unlink(socketPath) == 0;
}

static int createListenSocket(const char* socketPath) {
    if (strlen(socketPath) >= sizeof(((struct sockaddr_un*)0)->sun_path)) {
        fprintf(stderr, "vortek-cli: socket path is too long: %s\n", socketPath);
        return -1;
    }

    if (!createSocketDirectory(socketPath)) {
        fprintf(stderr, "vortek-cli: unable to create socket directory for %s: %s\n", socketPath, strerror(errno));
        return -1;
    }

    if (!removeExistingSocket(socketPath)) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "vortek-cli: socket failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socketPath);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "vortek-cli: bind failed for %s: %s\n", socketPath, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 16) != 0) {
        fprintf(stderr, "vortek-cli: listen failed: %s\n", strerror(errno));
        close(fd);
        unlink(socketPath);
        return -1;
    }

    return fd;
}

static void* runClient(void* param) {
    CliClient* client = param;
    VkContext* context = createVkContextForClient(client->clientFd, &client->contextOptions, NULL);
    if (!context) {
        fprintf(stderr, "vortek-cli: failed to create context for client fd %d\n", client->clientFd);
        close(client->clientFd);
        free(client);
        return NULL;
    }

    printf("vortek-cli: client connected fd=%d\n", client->clientFd);
    fflush(stdout);

    pthread_join(context->requestHandlerThread, NULL);
    context->requestHandlerThread = 0;
    destroyVkContext(NULL, context);
    close(client->clientFd);

    printf("vortek-cli: client disconnected fd=%d\n", client->clientFd);
    fflush(stdout);

    free(client);
    return NULL;
}

int main(int argc, char** argv) {
    CliConfig config = {0};
    char** exposedExtensions = NULL;
    int exposedExtensionCount = 0;

    if (!parseArgs(argc, argv, &config, &exposedExtensions, &exposedExtensionCount)) {
        printUsage(argv[0]);
        freeExposedExtensions(exposedExtensions, exposedExtensionCount);
        return 2;
    }

    if (!initVulkan(&config)) {
        freeExposedExtensions(exposedExtensions, exposedExtensionCount);
        return 1;
    }

    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    listenFd = createListenSocket(config.socketPath);
    if (listenFd < 0) {
        freeExposedExtensions(exposedExtensions, exposedExtensionCount);
        return 1;
    }

    printf("vortek-cli: listening on %s\n", config.socketPath);
    printf("vortek-cli: Vulkan API %u.%u.%u\n",
           VK_VERSION_MAJOR(config.contextOptions.vkMaxVersion),
           VK_VERSION_MINOR(config.contextOptions.vkMaxVersion),
           VK_VERSION_PATCH(config.contextOptions.vkMaxVersion));
    fflush(stdout);

    while (keepRunning) {
        int clientFd = accept(listenFd, NULL, NULL);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            if (!keepRunning) break;
            fprintf(stderr, "vortek-cli: accept failed: %s\n", strerror(errno));
            break;
        }

        CliClient* client = calloc(1, sizeof(CliClient));
        if (!client) {
            close(clientFd);
            continue;
        }

        client->clientFd = clientFd;
        client->contextOptions = config.contextOptions;

        pthread_t thread;
        if (pthread_create(&thread, NULL, runClient, client) != 0) {
            fprintf(stderr, "vortek-cli: failed to start client thread\n");
            close(clientFd);
            free(client);
            continue;
        }

        if (config.once) {
            pthread_join(thread, NULL);
            break;
        }

        pthread_detach(thread);
    }

    if (listenFd >= 0) {
        close(listenFd);
        listenFd = -1;
    }
    unlink(config.socketPath);
    freeExposedExtensions(exposedExtensions, exposedExtensionCount);
    return 0;
}
