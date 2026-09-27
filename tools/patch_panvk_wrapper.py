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
p.write_text(s)

p = Path("src/vulkan/wrapper/wrapper_physical_device.c")
s = p.read_text()
needle = """     WRAPPER_LOG(info, "GPU Name: %s", pdevice->properties2.properties.deviceName);"""
repl = """     WRAPPER_LOG(info, "PANVKWRAP PanVK-G57 integration build v1");
     WRAPPER_LOG(info, "GPU Name: %s", pdevice->properties2.properties.deviceName);"""
if needle not in s:
    raise SystemExit("physical device patch point not found")
p.write_text(s.replace(needle, repl, 1))
