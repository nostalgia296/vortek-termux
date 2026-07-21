#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "ring_buffer.h"
#include "time_utils.h"
#include "events.h"

#define STRUCT_OFFSETS() \
    struct Offsets { \
        atomic_uint head; \
        atomic_uint tail; \
        atomic_uint status; \
        atomic_uint readSignal; \
        atomic_uint writeSignal; \
        uint8_t buffer; \
    }

static int futexWait(atomic_uint* address, uint32_t expected) {
    return syscall(SYS_futex, (uint32_t*)address, FUTEX_WAIT, expected, NULL, NULL, 0);
}

static void futexWakeAll(atomic_uint* address) {
    syscall(SYS_futex, (uint32_t*)address, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}

static void RingBuffer_signalRead(RingBuffer* ring) {
    if (!ring || !ring->readSignal) return;
    atomic_fetch_add_explicit(ring->readSignal, 1, memory_order_release);
    futexWakeAll(ring->readSignal);
}

static void RingBuffer_signalWrite(RingBuffer* ring) {
    if (!ring || !ring->writeSignal) return;
    atomic_fetch_add_explicit(ring->writeSignal, 1, memory_order_release);
    futexWakeAll(ring->writeSignal);
}

static void RingBuffer_waitOn(atomic_uint* signal, uint32_t expected) {
    if (futexWait(signal, expected) < 0 && errno != EAGAIN && errno != EINTR) {
        uint32_t busyWaitIter = BUSY_WAIT_MAX_ITER;
        busyWait(&busyWaitIter);
    }
}

#ifdef __ANDROID__
#include <android/log.h>
#define debug_printf(...) __android_log_print(ANDROID_LOG_DEBUG, "System.out", __VA_ARGS__)
#else
#define debug_printf(...) fprintf(stderr, __VA_ARGS__)
#endif

void RingBuffer_setHead(RingBuffer* ring, uint32_t head) {
    atomic_store_explicit(ring->head, head, memory_order_release);
    RingBuffer_signalWrite(ring);
}

uint32_t RingBuffer_getHead(RingBuffer* ring) {
    return atomic_load_explicit(ring->head, memory_order_acquire);
}

void RingBuffer_setTail(RingBuffer* ring, uint32_t tail) {
    atomic_store_explicit(ring->tail, tail, memory_order_release);
    RingBuffer_signalRead(ring);
}

uint32_t RingBuffer_getTail(RingBuffer* ring) {
    return atomic_load_explicit(ring->tail, memory_order_acquire);
}

void RingBuffer_setStatus(RingBuffer* ring, uint32_t status) {
    atomic_fetch_or_explicit(ring->status, status, memory_order_seq_cst);
    if (status & RING_STATUS_EXIT) {
        RingBuffer_signalRead(ring);
        RingBuffer_signalWrite(ring);
    }
}

void RingBuffer_unsetStatus(RingBuffer* ring, uint32_t status) {
    atomic_fetch_and_explicit(ring->status, ~status, memory_order_seq_cst);
}

bool RingBuffer_hasStatus(RingBuffer* ring, uint32_t status) {
    return (atomic_load_explicit(ring->status, memory_order_seq_cst) & status);
}

RingBuffer* RingBuffer_create(int shmFd, uint32_t bufferSize) {
    RingBuffer* ring = calloc(1, sizeof(RingBuffer));
    if (!ring) return NULL;

    STRUCT_OFFSETS();

    int shmSize = RingBuffer_getSHMemSize(bufferSize);
    void* sharedData = mmap(NULL, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    if (sharedData == MAP_FAILED) {
        free(ring);
        return NULL;
    }
    memset(sharedData, 0, shmSize);

    ring->head = (atomic_uint*)((uint8_t*)sharedData + offsetof(struct Offsets, head));
    ring->tail = (atomic_uint*)((uint8_t*)sharedData + offsetof(struct Offsets, tail));
    ring->status = (atomic_uint*)((uint8_t*)sharedData + offsetof(struct Offsets, status));
    ring->readSignal = (atomic_uint*)((uint8_t*)sharedData + offsetof(struct Offsets, readSignal));
    ring->writeSignal = (atomic_uint*)((uint8_t*)sharedData + offsetof(struct Offsets, writeSignal));
    ring->buffer = (uint8_t*)sharedData + offsetof(struct Offsets, buffer);
    ring->sharedData = sharedData;
    ring->bufferSize = bufferSize;

    RingBuffer_setStatus(ring, RING_STATUS_IDLE);
    return ring;
}

uint32_t RingBuffer_size(RingBuffer* ring) {
    return RingBuffer_getTail(ring) - RingBuffer_getHead(ring);
}

uint32_t RingBuffer_freeSpace(RingBuffer* ring) {
    return ring->bufferSize - RingBuffer_size(ring);
}

bool RingBuffer_read(RingBuffer* ring, void* data, uint32_t size) {
    if (size > ring->bufferSize) {
        debug_printf("ring: buffer overflow on read (%d/%d)\n", size, ring->bufferSize);
        return false;
    }

    if (!RingBuffer_waitForRead(ring, size)) return false;
    uint32_t head = RingBuffer_getHead(ring);
    uint32_t offset = head & (ring->bufferSize - 1);

    if ((offset + size) <= ring->bufferSize) {
        memcpy(data, ring->buffer + offset, size);
    }
    else {
        uint32_t start = ring->bufferSize - offset;
        memcpy(data, ring->buffer + offset, start);
        memcpy(data + start, ring->buffer, size - start);
    }

    RingBuffer_setHead(ring, head + size);
    return true;
}

bool RingBuffer_write(RingBuffer* ring, const void* data, uint32_t size) {
    if (size > ring->bufferSize) {
        debug_printf("ring: buffer overflow on write (%d/%d)\n", size, ring->bufferSize);
        return false;
    }

    if (!RingBuffer_waitForWrite(ring, size)) return false;
    uint32_t tail = RingBuffer_getTail(ring);
    uint32_t offset = tail & (ring->bufferSize - 1);

    if ((offset + size) <= ring->bufferSize) {
        memcpy(ring->buffer + offset, data, size);
    }
    else {
        uint32_t start = ring->bufferSize - offset;
        memcpy(ring->buffer + offset, data, start);
        memcpy(ring->buffer, data + start, size - start);
    }

    RingBuffer_setTail(ring, tail + size);
    return true;
}

uint32_t RingBuffer_getSHMemSize(uint32_t bufferSize) {
    STRUCT_OFFSETS();

    return bufferSize + offsetof(struct Offsets, buffer);
}

void RingBuffer_free(RingBuffer* ring) {
    if (!ring) return;

    if (ring->sharedData) {
        RingBuffer_setStatus(ring, RING_STATUS_EXIT);
        munmap(ring->sharedData, RingBuffer_getSHMemSize(ring->bufferSize));
        ring->sharedData = NULL;
    }

    free(ring);
}

bool RingBuffer_waitForRead(RingBuffer* ring, uint32_t size) {
    do {
        if (RingBuffer_size(ring) >= size) break;
        if (RingBuffer_hasStatus(ring, RING_STATUS_EXIT)) return false;
        uint32_t signal = atomic_load_explicit(ring->readSignal, memory_order_acquire);
        if (RingBuffer_size(ring) >= size) break;
        if (RingBuffer_hasStatus(ring, RING_STATUS_EXIT)) return false;
        RingBuffer_waitOn(ring->readSignal, signal);
    }
    while (1);
    return true;
}

bool RingBuffer_waitForWrite(RingBuffer* ring, uint32_t size) {
    do {
        if (RingBuffer_freeSpace(ring) >= size) break;
        if (RingBuffer_hasStatus(ring, RING_STATUS_EXIT)) return false;
        uint32_t signal = atomic_load_explicit(ring->writeSignal, memory_order_acquire);
        if (RingBuffer_freeSpace(ring) >= size) break;
        if (RingBuffer_hasStatus(ring, RING_STATUS_EXIT)) return false;
        RingBuffer_waitOn(ring->writeSignal, signal);
    }
    while (1);
    return true;
}
