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

#ifndef __MM_JPEG_IONBUF_H__
#define __MM_JPEG_IONBUF_H__

// System dependencies
#include <linux/msm_ion.h>
#include <linux/ion.h>
#include <linux/dma-buf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
static_assert(sizeof(struct ion_allocation_data) == 24 &&
        offsetof(struct ion_allocation_data, fd) == 16,
        "The R11/R11s JPEG requires the modern ION fd-returning allocation ABI");
#else
_Static_assert(sizeof(struct ion_allocation_data) == 24 &&
        offsetof(struct ion_allocation_data, fd) == 16,
        "The R11/R11s JPEG requires the modern ION fd-returning allocation ABI");
#endif
// JPEG dependencies
#include "mm_jpeg_dbg.h"

typedef struct  {
  int p_pmem_fd;
  size_t size;
  size_t allocation_size;
  uint8_t *addr;
  bool owns_buffer;
} buffer_t;

/** buffer_allocate:
 *
 *  Arguments:
 *     @p_buffer: ION buffer
 *
 *  Return:
 *     buffer address
 *
 *  Description:
 *      allocates ION buffer
 *
 **/
void* buffer_allocate(buffer_t *p_buffer, int cached);

/** buffer_deallocate:
 *
 *  Arguments:
 *     @p_buffer: ION buffer
 *
 *  Return:
 *     error val
 *
 *  Description:
 *      deallocates ION buffer
 *
 **/
int buffer_deallocate(buffer_t *p_buffer);

/** buffer_invalidate:
 *
 *  Arguments:
 *     @p_buffer: ION buffer
 *
 *  Return:
 *     error val
 *
 *  Description:
 *      Invalidates the cached buffer
 *
 **/
int buffer_invalidate(buffer_t *p_buffer);

/** buffer_clean:
 *
 *  Arguments:
 *     @p_buffer: ION buffer
 *
 *  Return:
 *     error val
 *
 *  Description:
 *      clean the cached buffer
 *
 **/
int buffer_clean(buffer_t *p_buffer);

/** buffer_cache_ops:
 *
 *  Arguments:
 *     @p_buffer: ION buffer
 *     @uint32_t cmd
 *
 *  Return:
 *     error val
 *
 *  Description:
 *      buffer cache ops based on comd.
 *
 **/
int buffer_cache_ops(buffer_t *p_buffer, uint32_t cmd);

#endif
