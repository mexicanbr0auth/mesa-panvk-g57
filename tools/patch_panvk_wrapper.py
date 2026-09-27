from pathlib import Path

p = Path("src/vulkan/wrapper/wrapper_device.c")
s = p.read_text()

needle = """   result = physical_device->dispatch_table.CreateDevice(
      physical_device->dispatch_handle, &wrapper_create_info,
         pAllocator, &device->dispatch_handle);

   if (result != VK_SUCCESS) {"""
repl = """   WRAPPER_LOG(info, "PANVKWRAP CreateDevice underlying begin exts=%u",
               wrapper_enable_extension_count);
   result = physical_device->dispatch_table.CreateDevice(
      physical_device->dispatch_handle, &wrapper_create_info,
         pAllocator, &device->dispatch_handle);
   WRAPPER_LOG(info, "PANVKWRAP CreateDevice underlying end result=%d handle=%p",
               result, device->dispatch_handle);

   if (result != VK_SUCCESS) {"""
if needle not in s:
    raise SystemExit("CreateDevice patch point not found")
s = s.replace(needle, repl, 1)

needle = """         if (create_info->flags) {
            device->dispatch_table.GetDeviceQueue2("""
repl = """         WRAPPER_LOG(info, "PANVKWRAP queue init family=%u index=%d flags=0x%x",
                     create_info->queueFamilyIndex, j, create_info->flags);
         if (create_info->flags) {
            device->dispatch_table.GetDeviceQueue2("""
if needle not in s:
    raise SystemExit("queue patch point not found")
s = s.replace(needle, repl, 1)

needle = """         queue->device = device;

         result = vk_queue_init"""
repl = """         WRAPPER_LOG(info, "PANVKWRAP queue underlying handle=%p",
                     queue->dispatch_handle);
         queue->device = device;

         result = vk_queue_init"""
if needle not in s:
    raise SystemExit("queue result patch point not found")
s = s.replace(needle, repl, 1)
needle = """   result = queue->device->dispatch_table.QueueSubmit(
      queue->dispatch_handle, submitCount, wrapper_submits, fence);"""
repl = """   WRAPPER_LOG(info, "PANVKWRAP QueueSubmit begin count=%u queue=%p underlying=%p fence=%p",
               submitCount, (void *)_queue, (void *)queue->dispatch_handle, (void *)fence);
   for (uint32_t i = 0; i < submitCount; i++)
      WRAPPER_LOG(info, "PANVKWRAP QueueSubmit[%u] waits=%u cmds=%u signals=%u",
                  i, pSubmits[i].waitSemaphoreCount,
                  pSubmits[i].commandBufferCount,
                  pSubmits[i].signalSemaphoreCount);
   result = queue->device->dispatch_table.QueueSubmit(
      queue->dispatch_handle, submitCount, wrapper_submits, fence);
   WRAPPER_LOG(info, "PANVKWRAP QueueSubmit end result=%d", result);"""
if needle not in s:
    raise SystemExit("QueueSubmit patch point not found")
s = s.replace(needle, repl, 1)

needle = """   result = queue->device->dispatch_table.QueueSubmit2(
      queue->dispatch_handle, submitCount, wrapper_submits, fence);"""
repl = """   WRAPPER_LOG(info, "PANVKWRAP QueueSubmit2 begin count=%u queue=%p underlying=%p fence=%p",
               submitCount, (void *)_queue, (void *)queue->dispatch_handle, (void *)fence);
   for (uint32_t i = 0; i < submitCount; i++)
      WRAPPER_LOG(info, "PANVKWRAP QueueSubmit2[%u] waits=%u cmds=%u signals=%u",
                  i, pSubmits[i].waitSemaphoreInfoCount,
                  pSubmits[i].commandBufferInfoCount,
                  pSubmits[i].signalSemaphoreInfoCount);
   result = queue->device->dispatch_table.QueueSubmit2(
      queue->dispatch_handle, submitCount, wrapper_submits, fence);
   WRAPPER_LOG(info, "PANVKWRAP QueueSubmit2 end result=%d", result);"""
if needle not in s:
    raise SystemExit("QueueSubmit2 patch point not found")
s = s.replace(needle, repl, 1)

p.write_text(s)

p = Path("src/vulkan/wrapper/wrapper_physical_device.c")
s = p.read_text()
needle = """     WRAPPER_LOG(info, "GPU Name: %s", pdevice->properties2.properties.deviceName);"""
repl = """     WRAPPER_LOG(info, "PANVKWRAP PanVK-G57 integration build v1");
     WRAPPER_LOG(info, "GPU Name: %s", pdevice->properties2.properties.deviceName);"""
if needle not in s:
    raise SystemExit("physical device patch point not found")
p.write_text(s.replace(needle, repl, 1))


# PanVK-G57 X11/Present diagnostics: make Present protocol errors synchronous
# so they cannot escape later as a fatal asynchronous X error.
p = Path("src/vulkan/wsi/wsi_common_x11.c")
s = p.read_text()
needle = """   chain->event_id = xcb_generate_id(chain->conn);
   uint32_t event_mask = XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY |
                         XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY;
   if (!chain->base.image_info.explicit_sync)
      event_mask |= XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY;
   xcb_present_select_input(chain->conn, chain->event_id, chain->window, event_mask);

   /* Create an XCB event queue to hold present events outside of the usual
    * application event queue
    */
   chain->special_event =
      xcb_register_for_special_xge(chain->conn, &xcb_present_id,
                                   chain->event_id, NULL);"""
repl = """   chain->event_id = xcb_generate_id(chain->conn);
   uint32_t event_mask = XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY |
                         XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY;
   if (!chain->base.image_info.explicit_sync)
      event_mask |= XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY;
   xcb_void_cookie_t panvk_select_cookie =
      xcb_present_select_input_checked(chain->conn, chain->event_id,
                                       chain->window, event_mask);
   xcb_generic_error_t *panvk_select_error =
      xcb_request_check(chain->conn, panvk_select_cookie);
   if (panvk_select_error) {
      fprintf(stderr,
              \"PANVKWSI SelectInput CREATE failed err=%u major=%u minor=%u resource=0x%x seq=%u event=0x%x window=0x%x mask=0x%x\\n\",
              panvk_select_error->error_code, panvk_select_error->major_code,
              panvk_select_error->minor_code, panvk_select_error->resource_id,
              panvk_select_error->sequence, chain->event_id, chain->window,
              event_mask);
      free(panvk_select_error);
      result = VK_ERROR_SURFACE_LOST_KHR;
      goto fail_register;
   }
   fprintf(stderr,
           \"PANVKWSI SelectInput CREATE ok event=0x%x window=0x%x mask=0x%x\\n\",
           chain->event_id, chain->window, event_mask);

   /* Create an XCB event queue to hold present events outside of the usual
    * application event queue
    */
   chain->special_event =
      xcb_register_for_special_xge(chain->conn, &xcb_present_id,
                                   chain->event_id, NULL);
   if (!chain->special_event) {
      fprintf(stderr, \"PANVKWSI special XGE registration failed event=0x%x\\n\",
              chain->event_id);
      result = VK_ERROR_SURFACE_LOST_KHR;
      goto fail_register;
   }"""
if needle not in s:
    raise SystemExit("X11 SelectInput create patch point not found")
s = s.replace(needle, repl, 1)

needle = """   cookie = xcb_present_select_input_checked(chain->conn, chain->event_id,
                                             chain->window,
                                             XCB_PRESENT_EVENT_MASK_NO_EVENT);
   xcb_discard_reply(chain->conn, cookie.sequence);"""
repl = """   cookie = xcb_present_select_input_checked(chain->conn, chain->event_id,
                                             chain->window,
                                             XCB_PRESENT_EVENT_MASK_NO_EVENT);
   xcb_generic_error_t *panvk_cleanup_error = xcb_request_check(chain->conn, cookie);
   if (panvk_cleanup_error) {
      fprintf(stderr,
              \"PANVKWSI SelectInput CLEANUP failed err=%u major=%u minor=%u resource=0x%x seq=%u event=0x%x window=0x%x\\n\",
              panvk_cleanup_error->error_code, panvk_cleanup_error->major_code,
              panvk_cleanup_error->minor_code, panvk_cleanup_error->resource_id,
              panvk_cleanup_error->sequence, chain->event_id, chain->window);
      free(panvk_cleanup_error);
   } else {
      fprintf(stderr, \"PANVKWSI SelectInput CLEANUP ok event=0x%x window=0x%x\\n\",
              chain->event_id, chain->window);
   }"""
if needle not in s:
    raise SystemExit("X11 SelectInput cleanup patch point not found")
s = s.replace(needle, repl, 1)
p.write_text(s)

# Keep XID allocation coherent when WSI is using an Xlib Display shared with Wine.
p = Path("src/vulkan/wsi/wsi_common_x11.c")
s = p.read_text()
needle = """   xcb_connection_t *                           conn;
   xcb_window_t                                 window;"""
repl = """   xcb_connection_t *                           conn;
   Display *                                    xlib_dpy;
   xcb_window_t                                 window;"""
if needle not in s:
    raise SystemExit("x11_swapchain conn field patch point not found")
s = s.replace(needle, repl, 1)

needle = """};
VK_DEFINE_NONDISP_HANDLE_CASTS(x11_swapchain, base.base, VkSwapchainKHR,"""
repl = """};

static inline uint32_t
panvk_x11_generate_id(struct x11_swapchain *chain)
{
   if (chain->xlib_dpy) {
      uint32_t id = (uint32_t)XAllocID(chain->xlib_dpy);
      fprintf(stderr, \"PANVKWSI XID xlib=0x%x\\n\", id);
      return id;
   }

   uint32_t id = xcb_generate_id(chain->conn);
   fprintf(stderr, \"PANVKWSI XID xcb=0x%x\\n\", id);
   return id;
}

VK_DEFINE_NONDISP_HANDLE_CASTS(x11_swapchain, base.base, VkSwapchainKHR,"""
if needle not in s:
    raise SystemExit("x11_swapchain helper insertion point not found")
s = s.replace(needle, repl, 1)

# Every XID owned by the swapchain must use the same allocator.
s = s.replace("xcb_generate_id(chain->conn)", "panvk_x11_generate_id(chain)")
# The replacement above also touched the helper fallback; restore it there.
s = s.replace("uint32_t id = panvk_x11_generate_id(chain);\n   fprintf(stderr, \"PANVKWSI XID xcb=0x%x", "uint32_t id = xcb_generate_id(chain->conn);\n   fprintf(stderr, \"PANVKWSI XID xcb=0x%x", 1)

needle = """   chain->conn = conn;
   chain->window = window;"""
repl = """   chain->conn = conn;
   chain->xlib_dpy = icd_surface->platform == VK_ICD_WSI_PLATFORM_XLIB ?
      ((VkIcdSurfaceXlib *)icd_surface)->dpy : NULL;
   fprintf(stderr, \"PANVKWSI swapchain connection platform=%u xlib=%p conn=%p\\n\",
           icd_surface->platform, (void *)chain->xlib_dpy, (void *)chain->conn);
   chain->window = window;"""
if needle not in s:
    raise SystemExit("xlib display assignment patch point not found")
s = s.replace(needle, repl, 1)

# Make SYNC CreateFence synchronous too; this was the observed X request 152/14.
needle = """   image->sync_fence = panvk_x11_generate_id(chain);
   xcb_sync_create_fence(chain->conn, image->pixmap, image->sync_fence, false);
   xcb_sync_trigger_fence(chain->conn, image->sync_fence);
   
   return VK_SUCCESS;"""
repl = """   image->sync_fence = panvk_x11_generate_id(chain);
   cookie = xcb_sync_create_fence_checked(chain->conn, image->pixmap,
                                          image->sync_fence, false);
   error = xcb_request_check(chain->conn, cookie);
   if (error != NULL) {
      fprintf(stderr,
              \"PANVKWSI CreateFence failed err=%u major=%u minor=%u resource=0x%x seq=%u pixmap=0x%x fence=0x%x\\n\",
              error->error_code, error->major_code, error->minor_code,
              error->resource_id, error->sequence, image->pixmap,
              image->sync_fence);
      free(error);
      goto fail_image;
   }
   fprintf(stderr, \"PANVKWSI CreateFence ok pixmap=0x%x fence=0x%x\\n\",
           image->pixmap, image->sync_fence);
   xcb_sync_trigger_fence(chain->conn, image->sync_fence);

   return VK_SUCCESS;"""
if needle not in s:
    raise SystemExit("SYNC CreateFence patch point not found")
s = s.replace(needle, repl, 1)
p.write_text(s)
