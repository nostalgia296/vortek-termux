#include "vk_context.h"
#include "vulkan_helper.h"
#include "request_handler.h"
#include "sysvshared_memory.h"
#include "string_utils.h"
#include "jni_utils.h"

static bool loadJMethods(JMethods* jmethods) {
    if (!jmethods->jvm || !jmethods->obj) return false;

    JNIEnv* env;
    (*jmethods->jvm)->AttachCurrentThread(jmethods->jvm, &env, NULL);
    jmethods->env = env;

    jclass cls = (*env)->GetObjectClass(env, jmethods->obj);
    jmethods->getWindowWidth = (*env)->GetMethodID(env, cls, "getWindowWidth", "(I)I");
    jmethods->getWindowHeight = (*env)->GetMethodID(env, cls, "getWindowHeight", "(I)I");
    jmethods->getWindowHardwareBuffer = (*env)->GetMethodID(env, cls, "getWindowHardwareBuffer", "(IZ)J");
    jmethods->updateWindowContent = (*env)->GetMethodID(env, cls, "updateWindowContent", "(I)V");
    return true;
}

static ExtraDataRequest* waitForExtraDataRequest(VkContext* context, uint16_t requestId) {
    ExtraDataRequest* result = NULL;
    uint32_t busyWaitIter = 0;

    while (context->status >= 0) {
        result = NULL;
        pthread_mutex_lock(&context->extraDataRequestsMutex);
        for (int i = 0; i < context->extraDataRequests.size; i++) {
            ExtraDataRequest* extraDataRequest = context->extraDataRequests.elements[i];
            if (extraDataRequest->requestId == requestId) {
                result = extraDataRequest;
                ArrayList_removeAt(&context->extraDataRequests, i);
                break;
            }
        }
        pthread_mutex_unlock(&context->extraDataRequestsMutex);

        if (result) break;
        busyWait(&busyWaitIter);
    }

    return context->status >= 0 ? result : NULL;
}

static ExtraDataRequest* readExtraDataRequestFromSocket(VkContext* context, uint16_t requestId) {
    char header[HEADER_SIZE];
    int bytesRead = sock_read(context->clientFd, header, HEADER_SIZE);
    if (bytesRead != HEADER_SIZE) return NULL;

    int requestCode = *(int*)(header + 0);
    int requestLength = *(int*)(header + 4);
    int expectedRequestCode = PACK16(REQUEST_CODE_SEND_EXTRA_DATA, requestId);
    if (requestCode != expectedRequestCode || requestLength < 0) return NULL;

    ExtraDataRequest* extraDataRequest = calloc(1, sizeof(ExtraDataRequest));
    extraDataRequest->requestId = requestId;
    extraDataRequest->size = requestLength;

    if (requestLength > 0) {
        extraDataRequest->data = calloc(requestLength, 1);
        bytesRead = sock_read(context->clientFd, extraDataRequest->data, requestLength);
        if (bytesRead != requestLength) {
            MEMFREE(extraDataRequest->data);
            MEMFREE(extraDataRequest);
            return NULL;
        }
    }

    return extraDataRequest;
}

static void* requestHandlerThread(void* param) {
    VkContext* context = param;
    bool attachedToJvm = loadJMethods(&context->jmethods);
    ExtraDataRequest* extraDataRequest = NULL;

    while (context->status >= 0) {
        int requestCode = vt_recv(context->serverRing, &context->inputBuffer, &context->inputBufferSize, &context->memoryPool);
        if (requestCode < 0) break;

        if (requestCode > INT16_MAX) {
            uint16_t requestId = requestCode & 0xffff;
            requestCode = requestCode >> 16;
            extraDataRequest = attachedToJvm ? waitForExtraDataRequest(context, requestId) : readExtraDataRequestFromSocket(context, requestId);
            if (!extraDataRequest) break;
            context->inputBufferSize = extraDataRequest->size;
            context->inputBuffer = extraDataRequest->data;
        }

#if DEBUG_MODE
        println("handleRequest name=%s size=%d", requestCodeToString(requestCode), context->inputBufferSize);
#endif

        HandleRequestFunc handleRequestFunc = getHandleRequestFunc(requestCode);
        if (handleRequestFunc) handleRequestFunc(context);

        vt_free(&context->memoryPool);

        if (extraDataRequest) {
            MEMFREE(extraDataRequest->data);
            MEMFREE(extraDataRequest);
            extraDataRequest = NULL;
        }

        context->inputBuffer = NULL;
        context->inputBufferSize = 0;
    }

    if (attachedToJvm) (*context->jmethods.jvm)->DetachCurrentThread(context->jmethods.jvm);
    vt_free(&context->memoryPool);
    return NULL;
}

static bool setupRingBuffers(VkContext* context) {
    int shmFds[2] = {-1, -1};
    shmFds[0] = ashmemCreateRegion("vt-server-ring", RingBuffer_getSHMemSize(SERVER_RING_BUFFER_SIZE));
    shmFds[1] = ashmemCreateRegion("vt-client-ring", RingBuffer_getSHMemSize(CLIENT_RING_BUFFER_SIZE));
    if (shmFds[0] < 0 || shmFds[1] < 0) goto error;

    context->serverRing = RingBuffer_create(shmFds[0], SERVER_RING_BUFFER_SIZE);
    if (!context->serverRing) goto error;

    context->clientRing = RingBuffer_create(shmFds[1], CLIENT_RING_BUFFER_SIZE);
    if (!context->clientRing) goto error;

    int result = send_fds(context->clientFd, shmFds, 2, NULL, 0);
    CLOSEFD(shmFds[0]);
    CLOSEFD(shmFds[1]);

    if (result < 0) goto error;

    if (pthread_create(&context->requestHandlerThread, NULL, requestHandlerThread, context) != 0) goto error;
    return true;

error:
    CLOSEFD(shmFds[0]);
    CLOSEFD(shmFds[1]);
    if (context->serverRing) {
        RingBuffer_free(context->serverRing);
        context->serverRing = NULL;
    }
    if (context->clientRing) {
        RingBuffer_free(context->clientRing);
        context->clientRing = NULL;
    }
    return false;
}

VkContext* createVkContextForClient(int clientFd, const VkContextOptions* options, const JMethods* jmethods) {
    VkContext* context = calloc(1, sizeof(VkContext));
    context->clientFd = clientFd;
    context->vkMaxVersion = options ? options->vkMaxVersion : VK_API_VERSION_1_0;
    context->maxDeviceMemory = options ? options->maxDeviceMemory : 0;
    context->imageCacheSize = options ? options->imageCacheSize : 0;
    context->resourceMemoryType = options ? options->resourceMemoryType : RESOURCE_MEMORY_TYPE_AUTO;
    if (options && options->exposedDeviceExtensions && options->exposedDeviceExtensionCount > 0) {
        context->exposedDeviceExtensions = ArrayList_fromStrings(options->exposedDeviceExtensions, options->exposedDeviceExtensionCount);
    }
    if (jmethods) memcpy(&context->jmethods, jmethods, sizeof(JMethods));

    pthread_mutex_init(&context->extraDataRequestsMutex, NULL);

    context->memoryPool.data = calloc(MEMORY_POOL_MAX_SIZE, 1);
    context->threadPool = ThreadPool_init(THREAD_POOL_NUM_THREADS);

    if (!setupRingBuffers(context)) {
        destroyVkContext(NULL, context);
        return NULL;
    }

    return context;
}

VkContext* createVkContext(JNIEnv* env, jobject obj, int clientFd, jobject options) {
    jobjectArray exposedDeviceExtensions = getJFieldByName(env, options, "exposedDeviceExtensions", JSIGNATURE_ARRAY_STRING).l;
    ArrayList* exposedDeviceExtensionList = jstringArrayToCharArray(env, exposedDeviceExtensions);

    VkContextOptions contextOptions = {0};
    contextOptions.vkMaxVersion = getJFieldByName(env, options, "vkMaxVersion", "I").i;
    contextOptions.maxDeviceMemory = getJFieldByName(env, options, "maxDeviceMemory", "S").s;
    contextOptions.imageCacheSize = getJFieldByName(env, options, "imageCacheSize", "S").s;
    contextOptions.resourceMemoryType = getJFieldByName(env, options, "resourceMemoryType", "B").b;
    if (exposedDeviceExtensionList) {
        contextOptions.exposedDeviceExtensions = (const char* const*)exposedDeviceExtensionList->elements;
        contextOptions.exposedDeviceExtensionCount = exposedDeviceExtensionList->size;
    }

    JMethods jmethods = {0};
    (*env)->GetJavaVM(env, &jmethods.jvm);
    jmethods.obj = (*env)->NewGlobalRef(env, obj);

    VkContext* context = createVkContextForClient(clientFd, &contextOptions, &jmethods);
    ArrayList_free(exposedDeviceExtensionList, true);

    return context;
}

void destroyVkContext(JNIEnv* env, VkContext* context) {
    if (!context) return;

    context->status = VK_ERROR_DEVICE_LOST;

    if (context->requestHandlerThread) {
        RingBuffer_setStatus(context->serverRing, RING_STATUS_EXIT);
        RingBuffer_setStatus(context->clientRing, RING_STATUS_EXIT);
        pthread_join(context->requestHandlerThread, NULL);

        ThreadPool_destroy(context->threadPool);
        context->threadPool = NULL;

        context->requestHandlerThread = 0;
    }

    if (context->serverRing) {
        RingBuffer_free(context->serverRing);
        context->serverRing = NULL;
    }

    if (context->clientRing) {
        RingBuffer_free(context->clientRing);
        context->clientRing = NULL;
    }

    if (context->threadPool) {
        ThreadPool_destroy(context->threadPool);
        context->threadPool = NULL;
    }

    if (context->jmethods.obj) {
        JNIEnv* deleteEnv = env;
        bool attachedToJvm = false;
        if (!deleteEnv && context->jmethods.jvm) {
            attachedToJvm = (*context->jmethods.jvm)->AttachCurrentThread(context->jmethods.jvm, &deleteEnv, NULL) == 0;
        }
        if (deleteEnv) (*deleteEnv)->DeleteGlobalRef(deleteEnv, context->jmethods.obj);
        if (attachedToJvm) (*context->jmethods.jvm)->DetachCurrentThread(context->jmethods.jvm);
        context->jmethods.obj = NULL;
    }

    context->graphicsQueueIndex = 0;

    if (context->textureDecoder) {
        TextureDecoder_destroy(context->textureDecoder);
        context->textureDecoder = NULL;
    }

    ArrayList_free(context->exposedDeviceExtensions, true);
    context->exposedDeviceExtensions = NULL;

    ArrayList_free(context->disabledDeviceExtensions, true);
    context->disabledDeviceExtensions = NULL;

    ArrayList_free(&context->extraDataRequests, true);
    pthread_mutex_destroy(&context->extraDataRequestsMutex);

    MEMFREE(context->memoryPool.data);
    MEMFREE(context->engineName);
    free(context);
}

bool handleExtraDataRequest(VkContext* context, uint16_t requestId, int requestLength) {
    void* data = NULL;
    if (requestLength > 0) {
        data = calloc(requestLength, 1);
        int bytesRead = sock_read(context->clientFd, data, requestLength);
        if (bytesRead != requestLength) return false;
    }

    ExtraDataRequest* extraDataRequest = calloc(1, sizeof(ExtraDataRequest));
    extraDataRequest->requestId = requestId;
    extraDataRequest->size = requestLength;
    extraDataRequest->data = data;

    pthread_mutex_lock(&context->extraDataRequestsMutex);
    ArrayList_add(&context->extraDataRequests, extraDataRequest);
    pthread_mutex_unlock(&context->extraDataRequestsMutex);
    return true;
}
