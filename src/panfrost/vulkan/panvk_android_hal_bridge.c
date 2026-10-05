#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>
#include <vulkan/vulkan.h>
#include <android/log.h>

#define PANVK_HAL_LOG(...) __android_log_print(ANDROID_LOG_INFO, "PanVK-PPSSPP", __VA_ARGS__)

extern VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

static int
panvk_hal_close(struct hw_device_t *device)
{
   (void)device;
   return 0;
}

static VkResult
panvk_hal_EnumerateInstanceExtensionProperties(
   const char *pLayerName,
   uint32_t *pPropertyCount,
   VkExtensionProperties *pProperties)
{
   PANVK_HAL_LOG("HAL EnumerateInstanceExtensionProperties layer=%s", pLayerName ? pLayerName : "<null>");
   PFN_vkEnumerateInstanceExtensionProperties fn =
      (PFN_vkEnumerateInstanceExtensionProperties)
      vk_icdGetInstanceProcAddr(
         VK_NULL_HANDLE,
         "vkEnumerateInstanceExtensionProperties");

   if (!fn)
      return VK_ERROR_INITIALIZATION_FAILED;

   return fn(pLayerName, pPropertyCount, pProperties);
}

static VkResult
panvk_hal_CreateInstance(
   const VkInstanceCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator,
   VkInstance *pInstance)
{
   PANVK_HAL_LOG("HAL CreateInstance createInfo=%p allocator=%p", pCreateInfo, pAllocator);
   PFN_vkCreateInstance fn =
      (PFN_vkCreateInstance)
      vk_icdGetInstanceProcAddr(
         VK_NULL_HANDLE,
         "vkCreateInstance");

   if (!fn)
      return VK_ERROR_INITIALIZATION_FAILED;

   return fn(pCreateInfo, pAllocator, pInstance);
}

static PFN_vkVoidFunction
panvk_hal_GetInstanceProcAddr(
   VkInstance instance,
   const char *pName)
{
   PANVK_HAL_LOG("HAL GetInstanceProcAddr instance=%p name=%s", (void *)instance, pName ? pName : "<null>");
   return vk_icdGetInstanceProcAddr(instance, pName);
}

static hwvulkan_device_t panvk_hal_device = {
   .common = {
      .tag = HARDWARE_DEVICE_TAG,
      .version = HWVULKAN_DEVICE_API_VERSION_0_1,
      .module = NULL,
      .close = panvk_hal_close,
   },

   .EnumerateInstanceExtensionProperties =
      panvk_hal_EnumerateInstanceExtensionProperties,

   .CreateInstance =
      panvk_hal_CreateInstance,

   .GetInstanceProcAddr =
      panvk_hal_GetInstanceProcAddr,
};

static int
panvk_hal_module_open(
   const struct hw_module_t *module,
   const char *id,
   struct hw_device_t **device)
{
   (void)id;

   if (!device)
      return -1;

   PANVK_HAL_LOG("HAL module open module=%p id=%s", module, id ? id : "<null>");

   panvk_hal_device.common.module =
      (struct hw_module_t *)module;

   *device = &panvk_hal_device.common;

   return 0;
}

static struct hw_module_methods_t panvk_hal_module_methods = {
   .open = panvk_hal_module_open,
};

__attribute__((visibility("default")))
hwvulkan_module_t HAL_MODULE_INFO_SYM = {
   .common = {
      .tag = HARDWARE_MODULE_TAG,

      .module_api_version =
         HWVULKAN_MODULE_API_VERSION_0_1,

      .hal_api_version =
         HARDWARE_HAL_API_VERSION,

      .id =
         HWVULKAN_HARDWARE_MODULE_ID,

      .name =
         "Mesa PanVK Mali-G57 Vulkan HAL",

      .author =
         "Mesa",

      .methods =
         &panvk_hal_module_methods,
   },
};
