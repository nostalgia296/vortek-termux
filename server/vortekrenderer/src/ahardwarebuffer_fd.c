#include <android/hardware_buffer.h>

#include "native_handle.h"

extern const native_handle_t* AHardwareBuffer_getNativeHandle(const AHardwareBuffer* buffer);

int AHardwareBuffer_getFd(AHardwareBuffer* hardwareBuffer) {
    if (!hardwareBuffer) return -1;

    const native_handle_t* nativeHandle = AHardwareBuffer_getNativeHandle(hardwareBuffer);
    if (!nativeHandle || nativeHandle->numFds <= 0) return -1;

    return nativeHandle->data[0];
}
