from pathlib import Path
p=Path('src/vulkan/wsi/wsi_common_x11.c')
s=p.read_text()
old='''         AHardwareBuffer_sendHandleToUnixSocket(image->base.ahardware_buffer, sock_fds[0]);
         image->base.dma_buf_fd = sock_fds[1];
         image->base.drm_modifier = 1255;'''
new='''         int ahb_send = AHardwareBuffer_sendHandleToUnixSocket(
            image->base.ahardware_buffer, sock_fds[0]);
         fprintf(stderr,
                 "PANVKAHB SEND result=%d txfd=%d rxfd=%d ahb=%p\\n",
                 ahb_send, sock_fds[0], sock_fds[1],
                 (void *)image->base.ahardware_buffer);
         if (ahb_send != 0) {
            close(sock_fds[0]);
            close(sock_fds[1]);
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
         }
         image->base.dma_buf_fd = sock_fds[1];
         image->base.drm_modifier = 1255;'''
if old not in s: raise SystemExit('send patch point missing')
s=s.replace(old,new,1)
old='''         xcb_flush(chain->conn);
         read(sock_fds[0], &image->base.dma_buf_fd, 1);
         for (int i = 0; i < ARRAY_SIZE(sock_fds); i++) {'''
new='''         xcb_flush(chain->conn);
         const ssize_t ack = read(sock_fds[0], &image->base.dma_buf_fd, 1);
         fprintf(stderr,
                 "PANVKAHB ACK result=%zd errno=%d txfd=%d rxfd=%d\\n",
                 ack, ack < 0 ? errno : 0, sock_fds[0], sock_fds[1]);
         if (ack != 1) {
            for (int i = 0; i < ARRAY_SIZE(sock_fds); i++)
               close(sock_fds[i]);
            return VK_ERROR_SURFACE_LOST_KHR;
         }
         for (int i = 0; i < ARRAY_SIZE(sock_fds); i++) {'''
if old not in s: raise SystemExit('ack patch point missing')
s=s.replace(old,new,1)
p.write_text(s)
