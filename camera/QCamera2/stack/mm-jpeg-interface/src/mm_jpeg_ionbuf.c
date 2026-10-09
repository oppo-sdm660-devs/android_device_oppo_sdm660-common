/* Copyright (c) 2013-2019, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

// System dependencies
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define MMAN_H <SYSTEM_HEADER_PREFIX/mman.h>
#include MMAN_H

// JPEG dependencies
#include "mm_jpeg_ionbuf.h"

#ifndef CAM_CACHE_OPS
#define CAM_CACHE_OPS
enum {
    CAM_CLEAN_CACHE,
    CAM_INV_CACHE,
    CAM_CLEAN_INV_CACHE
};
#define ION_IOC_CLEAN_CACHES CAM_CLEAN_CACHE
#define ION_IOC_INV_CACHES CAM_INV_CACHE
#define ION_IOC_CLEAN_INV_CACHES CAM_CLEAN_INV_CACHE
#endif

/* The dma-buf fd is owned separately from the allocator fd. */
void *buffer_allocate(buffer_t *p_buffer, int cached)
{
    const size_t alignment = 4096;
    if (p_buffer == NULL || p_buffer->owns_buffer || p_buffer->size == 0 ||
            p_buffer->size > (size_t)SSIZE_MAX - (alignment - 1)) {
        return NULL;
    }
    const size_t allocation_size =
            (p_buffer->size + alignment - 1) & ~(alignment - 1);
    p_buffer->p_pmem_fd = -1;
    p_buffer->addr = NULL;
    p_buffer->allocation_size = 0;
    p_buffer->owns_buffer = false;

    const int allocator_fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
    if (allocator_fd < 0) {
        LOGE("Ion open failed: %s", strerror(errno));
        return NULL;
    }
    struct ion_allocation_data allocation = {0};
    allocation.len = allocation_size;
    allocation.heap_id_mask = ION_HEAP(ION_SYSTEM_HEAP_ID);
    allocation.flags = cached ? ION_FLAG_CACHED : 0;
    int rc;
    do {
        rc = ioctl(allocator_fd, ION_IOC_ALLOC, &allocation);
    } while (rc < 0 && errno == EINTR);
    const int allocation_error = errno;
    close(allocator_fd);
    if (rc < 0) {
        LOGE("ION allocation failed for len %zu: %s", allocation_size,
                strerror(allocation_error));
        return NULL;
    }
    if (allocation.fd > (uint32_t)INT_MAX) {
        LOGE("ION returned an invalid dma-buf descriptor");
        return NULL;
    }
    const int dma_fd = (int)allocation.fd;
    void *mapping = mmap(NULL, allocation_size, PROT_READ | PROT_WRITE,
            MAP_SHARED, dma_fd, 0);
    if (mapping == MAP_FAILED) {
        const int mapping_error = errno;
        close(dma_fd);
        LOGE("ION mmap failed: %s", strerror(mapping_error));
        return NULL;
    }
    p_buffer->p_pmem_fd = dma_fd;
    p_buffer->addr = mapping;
    p_buffer->allocation_size = allocation_size;
    p_buffer->owns_buffer = true;
    return mapping;
}

int buffer_deallocate(buffer_t *p_buffer)
{
    if (p_buffer == NULL) {
        return -EINVAL;
    }
    if (!p_buffer->owns_buffer) {
        return 0;
    }
    int result = 0;
    if (p_buffer->addr != NULL && p_buffer->allocation_size != 0 &&
            munmap(p_buffer->addr, p_buffer->allocation_size) < 0) {
        result = -errno;
    }
    if (p_buffer->p_pmem_fd >= 0 && close(p_buffer->p_pmem_fd) < 0 && result == 0) {
        result = -errno;
    }
    p_buffer->addr = NULL;
    p_buffer->p_pmem_fd = -1;
    p_buffer->allocation_size = 0;
    p_buffer->owns_buffer = false;
    return result;
}

int buffer_invalidate(buffer_t *p_buffer)
{
    return buffer_cache_ops(p_buffer, CAM_INV_CACHE);
}

int buffer_clean(buffer_t *p_buffer)
{
    return buffer_cache_ops(p_buffer, CAM_CLEAN_CACHE);
}

int buffer_cache_ops(buffer_t *p_buffer, uint32_t cmd)
{
    if (p_buffer == NULL || p_buffer->p_pmem_fd < 0) {
        return -EINVAL;
    }
    uint64_t access;
    switch (cmd) {
    case CAM_INV_CACHE:
        access = DMA_BUF_SYNC_READ;
        break;
    case CAM_CLEAN_CACHE:
        access = DMA_BUF_SYNC_WRITE;
        break;
    case CAM_CLEAN_INV_CACHE:
        access = DMA_BUF_SYNC_RW;
        break;
    default:
        return -EINVAL;
    }

    struct dma_buf_sync sync = {0};
    sync.flags = DMA_BUF_SYNC_START | access;
    int rc;
    do {
        rc = ioctl(p_buffer->p_pmem_fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (rc < 0 && (errno == EINTR || errno == EAGAIN));
    if (rc < 0) {
        const int sync_error = errno;
        LOGE("DMA_BUF_IOCTL_SYNC start failed: %s", strerror(sync_error));
        return -sync_error;
    }
    sync.flags = DMA_BUF_SYNC_END | access;
    do {
        rc = ioctl(p_buffer->p_pmem_fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (rc < 0 && (errno == EINTR || errno == EAGAIN));
    if (rc < 0) {
        const int sync_error = errno;
        LOGE("DMA_BUF_IOCTL_SYNC end failed: %s", strerror(sync_error));
        return -sync_error;
    }
    return 0;
}
