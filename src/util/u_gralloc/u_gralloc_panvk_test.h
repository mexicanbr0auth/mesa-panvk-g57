/* SPDX-License-Identifier: MIT */
#ifndef U_GRALLOC_PANVK_TEST_H
#define U_GRALLOC_PANVK_TEST_H

#include <cutils/native_handle.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util/log.h"

/* Diagnostic override for the observed G57 RGB handles only.
 * This does not discover the gralloc layout or identify arbitrary dma-bufs.
 * Never close or reorder the handle's borrowed file descriptors.
 */
static inline int
u_gralloc_panvk_test_fd(const native_handle_t *handle, const char *where)
{
   if (!handle || handle->numFds < 1)
      return -1;

   const char *test = getenv("PANVK_TEST_AHB_FD1");
   if (!test || strcmp(test, "1") != 0)
      return handle->data[0];

   if (handle->numFds != 3) {
      mesa_loge("AHBFD %s: expected 3 fds, got %d", where, handle->numFds);
      return -1;
   }

   int fd = handle->data[1];
   off_t size = lseek(fd, 0, SEEK_END);
   if (size <= 0) {
      mesa_loge("AHBFD %s: index=1 fd=%d invalid size=%lld",
                where, fd, (long long)size);
      return -1;
   }

   mesa_logi("AHBFD %s: index=1 fd=%d size=%lld",
             where, fd, (long long)size);
   return fd;
}

#endif
