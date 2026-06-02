//
// vk_init.c - Vulkan instance/device/swapchain setup.
//
// Session 1 of the H2R Vulkan port: get a swapchain up, clear to a
// solid colour, present. No drawing primitives yet.
//

#include "vk_local.h"
#include "vk_buffer.h"
#include "vk_pipeline.h"
#include "vk_pipeline3d.h"
#include "vk_pipeline_world.h"
#include "vk_image.h"
#include "vk_draw.h"
#include "vk_world.h"
#include "vk_model.h"
#include "vk_sprite.h"
#include "vk_engine_model.h"
#include "vk_particles.h"
#include "vk_sky.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

vk_state_t vk_state = { 0 };

// ---------------------------------------------------------------------------
// Validation layer wiring
// ---------------------------------------------------------------------------

static const char* k_validation_layers[] = {
    "VK_LAYER_KHRONOS_validation",
};

static VKAPI_ATTR VkBool32 VKAPI_CALL VK_DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT        type,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*                                   userdata)
{
    (void)type; (void)userdata;
    const char* sev = "info";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)        sev = "ERROR";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) sev = "WARN";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)    sev = "info";
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) return VK_FALSE;
    ri.Con_Printf(PRINT_ALL, "vk[%s]: %s\n", sev, data->pMessage);
    return VK_FALSE;
}

static qboolean VK_CheckValidationLayerSupport(void)
{
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, NULL);
    if (count == 0) return false;

    VkLayerProperties* props = malloc(count * sizeof(*props));
    vkEnumerateInstanceLayerProperties(&count, props);

    qboolean found = false;
    for (uint32_t i = 0; i < count && !found; i++) {
        if (strcmp(props[i].layerName, k_validation_layers[0]) == 0)
            found = true;
    }
    free(props);
    return found;
}

// ---------------------------------------------------------------------------
// Instance creation
// ---------------------------------------------------------------------------

static qboolean VK_CreateInstance(void)
{
    VkApplicationInfo app = {0};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "Heretic2R";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName        = "H2R";
    app.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion         = VK_API_VERSION_1_2;

    // SDL3 tells us what instance extensions we need for surface creation.
    uint32_t sdl_ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if (!sdl_exts) {
        ri.Con_Printf(PRINT_ALL, "vk: SDL_Vulkan_GetInstanceExtensions failed: %s\n", SDL_GetError());
        return false;
    }

    // Allocate space for SDL extensions + optional debug-utils extension.
    const char** exts = malloc((sdl_ext_count + 1) * sizeof(*exts));
    for (uint32_t i = 0; i < sdl_ext_count; i++) exts[i] = sdl_exts[i];
    uint32_t ext_count = sdl_ext_count;

    vk_state.validation_enabled = VK_CheckValidationLayerSupport();
    if (vk_state.validation_enabled) {
        exts[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        ri.Con_Printf(PRINT_ALL, "vk: validation layers enabled\n");
    }

    VkInstanceCreateInfo ci = {0};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app;
    ci.enabledExtensionCount   = ext_count;
    ci.ppEnabledExtensionNames = exts;

    if (vk_state.validation_enabled) {
        ci.enabledLayerCount   = 1;
        ci.ppEnabledLayerNames = k_validation_layers;
    }

    VkResult r = vkCreateInstance(&ci, NULL, &vk_state.instance);
    free(exts);

    if (r != VK_SUCCESS) {
        ri.Con_Printf(PRINT_ALL, "vk: vkCreateInstance failed (%d)\n", r);
        return false;
    }

    // Set up debug messenger so we get validation prints in our console.
    if (vk_state.validation_enabled) {
        PFN_vkCreateDebugUtilsMessengerEXT pfn =
            (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(vk_state.instance, "vkCreateDebugUtilsMessengerEXT");
        if (pfn) {
            VkDebugUtilsMessengerCreateInfoEXT dci = {0};
            dci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dci.pfnUserCallback = VK_DebugCallback;
            pfn(vk_state.instance, &dci, NULL, &vk_state.debug_messenger);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Physical device selection
// ---------------------------------------------------------------------------

static qboolean VK_FindQueueFamilies(VkPhysicalDevice pdev, VkSurfaceKHR surf,
                                     uint32_t* gfx_out, uint32_t* present_out)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &count, NULL);
    if (count == 0) return false;

    VkQueueFamilyProperties* props = malloc(count * sizeof(*props));
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &count, props);

    uint32_t gfx = UINT32_MAX, present = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            if (gfx == UINT32_MAX) gfx = i;

        VkBool32 supports_present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pdev, i, surf, &supports_present);
        if (supports_present && present == UINT32_MAX) present = i;

        if (gfx != UINT32_MAX && present != UINT32_MAX) break;
    }
    free(props);

    if (gfx == UINT32_MAX || present == UINT32_MAX) return false;
    *gfx_out = gfx;
    *present_out = present;
    return true;
}

static qboolean VK_PickPhysicalDevice(void)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(vk_state.instance, &count, NULL);
    if (count == 0) {
        ri.Con_Printf(PRINT_ALL, "vk: no physical devices with Vulkan support\n");
        return false;
    }

    VkPhysicalDevice* devs = malloc(count * sizeof(*devs));
    vkEnumeratePhysicalDevices(vk_state.instance, &count, devs);

    // Pick discrete GPU if available, otherwise first integrated.
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t chosen_gfx = 0, chosen_present = 0;
    for (uint32_t pass = 0; pass < 2 && chosen == VK_NULL_HANDLE; pass++) {
        VkPhysicalDeviceType want = (pass == 0)
            ? VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
            : VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        for (uint32_t i = 0; i < count; i++) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devs[i], &p);
            if (p.deviceType != want && pass != 1) continue;
            uint32_t g, pr;
            if (VK_FindQueueFamilies(devs[i], vk_state.surface, &g, &pr)) {
                chosen = devs[i];
                chosen_gfx = g; chosen_present = pr;
                break;
            }
        }
    }

    // Final fallback: literally any device.
    if (chosen == VK_NULL_HANDLE) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t g, pr;
            if (VK_FindQueueFamilies(devs[i], vk_state.surface, &g, &pr)) {
                chosen = devs[i];
                chosen_gfx = g; chosen_present = pr;
                break;
            }
        }
    }
    free(devs);

    if (chosen == VK_NULL_HANDLE) {
        ri.Con_Printf(PRINT_ALL, "vk: no suitable physical device\n");
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(chosen, &props);
    ri.Con_Printf(PRINT_ALL, "vk: using GPU '%s' (api %u.%u.%u)\n",
                  props.deviceName,
                  VK_VERSION_MAJOR(props.apiVersion),
                  VK_VERSION_MINOR(props.apiVersion),
                  VK_VERSION_PATCH(props.apiVersion));

    vk_state.physical_device       = chosen;
    vk_state.graphics_queue_family = chosen_gfx;
    vk_state.present_queue_family  = chosen_present;
    return true;
}

// ---------------------------------------------------------------------------
// Logical device + queues
// ---------------------------------------------------------------------------

static qboolean VK_CreateDevice(void)
{
    const float priority = 1.0f;

    // If graphics and present queues are the same family, we only need one
    // VkDeviceQueueCreateInfo entry; otherwise we need two.
    VkDeviceQueueCreateInfo qci[2] = {0};
    uint32_t qcount = 0;

    qci[qcount].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci[qcount].queueFamilyIndex = vk_state.graphics_queue_family;
    qci[qcount].queueCount       = 1;
    qci[qcount].pQueuePriorities = &priority;
    qcount++;

    if (vk_state.present_queue_family != vk_state.graphics_queue_family) {
        qci[qcount].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci[qcount].queueFamilyIndex = vk_state.present_queue_family;
        qci[qcount].queueCount       = 1;
        qci[qcount].pQueuePriorities = &priority;
        qcount++;
    }

    const char* device_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkPhysicalDeviceFeatures features = {0};
    // No features required for Session 1.

    VkDeviceCreateInfo ci = {0};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = qcount;
    ci.pQueueCreateInfos       = qci;
    ci.enabledExtensionCount   = 1;
    ci.ppEnabledExtensionNames = device_exts;
    ci.pEnabledFeatures        = &features;

    VkResult r = vkCreateDevice(vk_state.physical_device, &ci, NULL, &vk_state.device);
    if (r != VK_SUCCESS) {
        ri.Con_Printf(PRINT_ALL, "vk: vkCreateDevice failed (%d)\n", r);
        return false;
    }

    vkGetDeviceQueue(vk_state.device, vk_state.graphics_queue_family, 0, &vk_state.graphics_queue);
    vkGetDeviceQueue(vk_state.device, vk_state.present_queue_family,  0, &vk_state.present_queue);
    return true;
}

// ---------------------------------------------------------------------------
// Swapchain creation
// ---------------------------------------------------------------------------

static VkSurfaceFormatKHR VK_ChooseSurfaceFormat(void)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_state.physical_device, vk_state.surface, &count, NULL);

    VkSurfaceFormatKHR* fmts = malloc(count * sizeof(*fmts));
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_state.physical_device, vk_state.surface, &count, fmts);

    // Prefer 32-bit BGRA in non-linear sRGB.
    VkSurfaceFormatKHR chosen = fmts[0];
    for (uint32_t i = 0; i < count; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM
         && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = fmts[i];
            break;
        }
    }
    free(fmts);
    return chosen;
}

static VkPresentModeKHR VK_ChoosePresentMode(void)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk_state.physical_device, vk_state.surface, &count, NULL);

    VkPresentModeKHR* modes = malloc(count * sizeof(*modes));
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk_state.physical_device, vk_state.surface, &count, modes);

    // FIFO is guaranteed; MAILBOX gives low-latency vsync if available.
    VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < count; i++) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
            chosen = modes[i];
            break;
        }
    }
    free(modes);
    return chosen;
}

static qboolean VK_CreateSwapchain(int width, int height)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_state.physical_device, vk_state.surface, &caps);

    VkSurfaceFormatKHR fmt = VK_ChooseSurfaceFormat();
    VkPresentModeKHR   pm  = VK_ChoosePresentMode();

    VkExtent2D extent;
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        extent.width  = (uint32_t)width;
        extent.height = (uint32_t)height;
        if (extent.width  < caps.minImageExtent.width)  extent.width  = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.width  > caps.maxImageExtent.width)  extent.width  = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = {0};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = vk_state.surface;
    ci.minImageCount    = image_count;
    ci.imageFormat      = fmt.format;
    ci.imageColorSpace  = fmt.colorSpace;
    ci.imageExtent      = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t queue_indices[2] = { vk_state.graphics_queue_family, vk_state.present_queue_family };
    if (vk_state.graphics_queue_family != vk_state.present_queue_family) {
        ci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices   = queue_indices;
    } else {
        ci.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE;
    }
    ci.preTransform   = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode    = pm;
    ci.clipped        = VK_TRUE;
    ci.oldSwapchain   = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(vk_state.device, &ci, NULL, &vk_state.swapchain) != VK_SUCCESS) {
        ri.Con_Printf(PRINT_ALL, "vk: vkCreateSwapchainKHR failed\n");
        return false;
    }

    vk_state.swapchain_format = fmt.format;
    vk_state.swapchain_extent = extent;

    vkGetSwapchainImagesKHR(vk_state.device, vk_state.swapchain, &vk_state.swapchain_image_count, NULL);
    vk_state.swapchain_images = calloc(vk_state.swapchain_image_count, sizeof(VkImage));
    vk_state.swapchain_image_views = calloc(vk_state.swapchain_image_count, sizeof(VkImageView));
    vk_state.swapchain_framebuffers = calloc(vk_state.swapchain_image_count, sizeof(VkFramebuffer));
    vkGetSwapchainImagesKHR(vk_state.device, vk_state.swapchain, &vk_state.swapchain_image_count, vk_state.swapchain_images);

    for (uint32_t i = 0; i < vk_state.swapchain_image_count; i++) {
        VkImageViewCreateInfo vci = {0};
        vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image    = vk_state.swapchain_images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = fmt.format;
        vci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel   = 0;
        vci.subresourceRange.levelCount     = 1;
        vci.subresourceRange.baseArrayLayer = 0;
        vci.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(vk_state.device, &vci, NULL, &vk_state.swapchain_image_views[i]) != VK_SUCCESS) {
            ri.Con_Printf(PRINT_ALL, "vk: vkCreateImageView failed (%u)\n", i);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Depth buffer
// ---------------------------------------------------------------------------

static VkFormat VK_PickDepthFormat(void)
{
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D16_UNORM,
    };
    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(vk_state.physical_device, candidates[i], &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return candidates[i];
    }
    return VK_FORMAT_UNDEFINED;
}

static qboolean VK_CreateDepthResources(void)
{
    vk_state.depth_format = VK_PickDepthFormat();
    if (vk_state.depth_format == VK_FORMAT_UNDEFINED) {
        ri.Con_Printf(PRINT_ALL, "vk: no supported depth format\n");
        return false;
    }

    VkImageCreateInfo ici = {0};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = vk_state.depth_format;
    ici.extent.width  = vk_state.swapchain_extent.width;
    ici.extent.height = vk_state.swapchain_extent.height;
    ici.extent.depth  = 1;
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(vk_state.device, &ici, NULL, &vk_state.depth_image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk_state.device, vk_state.depth_image, &req);

    VkMemoryAllocateInfo ai = {0};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    // Local helper from vk_buffer.c via extern decl below.
    extern uint32_t VK_FindMemoryType(uint32_t, VkMemoryPropertyFlags);
    ai.memoryTypeIndex = VK_FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vk_state.device, &ai, NULL, &vk_state.depth_memory) != VK_SUCCESS)
        return false;
    vkBindImageMemory(vk_state.device, vk_state.depth_image, vk_state.depth_memory, 0);

    VkImageViewCreateInfo vci = {0};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = vk_state.depth_image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = vk_state.depth_format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vk_state.device, &vci, NULL, &vk_state.depth_view) != VK_SUCCESS)
        return false;
    return true;
}

static void VK_DestroyDepthResources(void)
{
    if (vk_state.depth_view)   vkDestroyImageView(vk_state.device, vk_state.depth_view,   NULL);
    if (vk_state.depth_image)  vkDestroyImage    (vk_state.device, vk_state.depth_image,  NULL);
    if (vk_state.depth_memory) vkFreeMemory      (vk_state.device, vk_state.depth_memory, NULL);
    vk_state.depth_view = VK_NULL_HANDLE;
    vk_state.depth_image = VK_NULL_HANDLE;
    vk_state.depth_memory = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Render pass + framebuffers
// ---------------------------------------------------------------------------

static qboolean VK_CreateRenderPass(void)
{
    VkAttachmentDescription attachments[2] = {0};

    // [0] colour
    attachments[0].format         = vk_state.swapchain_format;
    attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // [1] depth
    attachments[1].format         = vk_state.depth_format;
    attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = {0};
    color_ref.attachment = 0;
    color_ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_ref = {0};
    depth_ref.attachment = 1;
    depth_ref.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount    = 1;
    sub.pColorAttachments       = &color_ref;
    sub.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency dep = {0};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                      | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                      | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                      | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo ci = {0};
    ci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = 2;
    ci.pAttachments    = attachments;
    ci.subpassCount    = 1;
    ci.pSubpasses      = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies   = &dep;

    if (vkCreateRenderPass(vk_state.device, &ci, NULL, &vk_state.render_pass) != VK_SUCCESS) {
        ri.Con_Printf(PRINT_ALL, "vk: vkCreateRenderPass failed\n");
        return false;
    }
    return true;
}

static qboolean VK_CreateFramebuffers(void)
{
    for (uint32_t i = 0; i < vk_state.swapchain_image_count; i++) {
        VkImageView attachments[2] = {
            vk_state.swapchain_image_views[i],
            vk_state.depth_view,
        };
        VkFramebufferCreateInfo ci = {0};
        ci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        ci.renderPass      = vk_state.render_pass;
        ci.attachmentCount = 2;
        ci.pAttachments    = attachments;
        ci.width           = vk_state.swapchain_extent.width;
        ci.height          = vk_state.swapchain_extent.height;
        ci.layers          = 1;
        if (vkCreateFramebuffer(vk_state.device, &ci, NULL, &vk_state.swapchain_framebuffers[i]) != VK_SUCCESS) {
            ri.Con_Printf(PRINT_ALL, "vk: vkCreateFramebuffer failed (%u)\n", i);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Command pool + command buffers + sync objects
// ---------------------------------------------------------------------------

static qboolean VK_CreateCommandPool(void)
{
    VkCommandPoolCreateInfo ci = {0};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = vk_state.graphics_queue_family;

    if (vkCreateCommandPool(vk_state.device, &ci, NULL, &vk_state.command_pool) != VK_SUCCESS) {
        ri.Con_Printf(PRINT_ALL, "vk: vkCreateCommandPool failed\n");
        return false;
    }

    VkCommandBufferAllocateInfo ai = {0};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = vk_state.command_pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    if (vkAllocateCommandBuffers(vk_state.device, &ai, vk_state.command_buffers) != VK_SUCCESS) {
        ri.Con_Printf(PRINT_ALL, "vk: vkAllocateCommandBuffers failed\n");
        return false;
    }
    return true;
}

static qboolean VK_CreateSyncObjects(void)
{
    VkSemaphoreCreateInfo sci = {0};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // start signalled so the first frame doesn't block

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(vk_state.device, &sci, NULL, &vk_state.image_available[i]) != VK_SUCCESS ||
            vkCreateSemaphore(vk_state.device, &sci, NULL, &vk_state.render_finished[i]) != VK_SUCCESS ||
            vkCreateFence    (vk_state.device, &fci, NULL, &vk_state.in_flight_fences[i]) != VK_SUCCESS) {
            ri.Con_Printf(PRINT_ALL, "vk: failed to create sync objects\n");
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Top-level init / shutdown
// ---------------------------------------------------------------------------

static void VK_CleanupSwapchain(void)
{
    if (vk_state.swapchain_framebuffers) {
        for (uint32_t i = 0; i < vk_state.swapchain_image_count; i++)
            if (vk_state.swapchain_framebuffers[i])
                vkDestroyFramebuffer(vk_state.device, vk_state.swapchain_framebuffers[i], NULL);
        free(vk_state.swapchain_framebuffers);
        vk_state.swapchain_framebuffers = NULL;
    }
    VK_DestroyDepthResources();
    if (vk_state.swapchain_image_views) {
        for (uint32_t i = 0; i < vk_state.swapchain_image_count; i++)
            if (vk_state.swapchain_image_views[i])
                vkDestroyImageView(vk_state.device, vk_state.swapchain_image_views[i], NULL);
        free(vk_state.swapchain_image_views);
        vk_state.swapchain_image_views = NULL;
    }
    free(vk_state.swapchain_images);
    vk_state.swapchain_images = NULL;
    vk_state.swapchain_image_count = 0;
    if (vk_state.swapchain) {
        vkDestroySwapchainKHR(vk_state.device, vk_state.swapchain, NULL);
        vk_state.swapchain = VK_NULL_HANDLE;
    }
}

qboolean VK_InitContext(SDL_Window* window)
{
    if (vk_state.initialized) return true;

    vk_state.window = window;
    SDL_GetWindowSize(window, &vk_state.window_width, &vk_state.window_height);

    // SDL3 lazily loads the Vulkan library when needed; calling LoadLibrary
    // explicitly makes any failure show up before we start creating Vulkan
    // objects (which would otherwise fault inside loader stubs).
    if (!SDL_Vulkan_LoadLibrary(NULL)) {
        ri.Con_Printf(PRINT_ALL, "vk: SDL_Vulkan_LoadLibrary failed: %s\n", SDL_GetError());
        return false;
    }

    if (!VK_CreateInstance()) return false;

    if (!SDL_Vulkan_CreateSurface(window, vk_state.instance, NULL, &vk_state.surface)) {
        ri.Con_Printf(PRINT_ALL, "vk: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return false;
    }
    if (!VK_PickPhysicalDevice())          return false;
    if (!VK_CreateDevice())                return false;
    if (!VK_CreateSwapchain(vk_state.window_width, vk_state.window_height)) return false;
    if (!VK_CreateDepthResources())        return false;
    if (!VK_CreateRenderPass())            return false;
    if (!VK_CreateFramebuffers())          return false;
    if (!VK_CreateCommandPool())           return false;
    if (!VK_CreateSyncObjects())           return false;

    if (!VK_CreatePipeline2D()) {
        ri.Con_Printf(PRINT_ALL, "vk: VK_CreatePipeline2D failed\n");
        return false;
    }
    // World before 3D: the world pipeline creates the shared dynamic-light
    // descriptor set layout (set = 1) that the entity (3D) pipeline also uses.
    if (!VK_CreatePipelineWorld()) {
        ri.Con_Printf(PRINT_ALL, "vk: VK_CreatePipelineWorld failed\n");
        return false;
    }
    if (!VK_CreatePipeline3D()) {
        ri.Con_Printf(PRINT_ALL, "vk: VK_CreatePipeline3D failed\n");
        return false;
    }
    if (!VK_InitImages()) {
        ri.Con_Printf(PRINT_ALL, "vk: VK_InitImages failed\n");
        return false;
    }
    if (!VK_DrawInit()) {
        ri.Con_Printf(PRINT_ALL, "vk: VK_DrawInit failed\n");
        return false;
    }
    if (!VK_Particles_Init()) {
        ri.Con_Printf(PRINT_ALL, "vk: VK_Particles_Init failed\n");
        // Non-fatal.
    }

    vk_state.initialized = true;
    ri.Con_Printf(PRINT_ALL, "vk: initialized %dx%d, %u swapchain images\n",
                  vk_state.swapchain_extent.width, vk_state.swapchain_extent.height,
                  vk_state.swapchain_image_count);
    return true;
}

qboolean VK_RecreateSwapchain(int width, int height)
{
    if (!vk_state.initialized) return false;
    vkDeviceWaitIdle(vk_state.device);

    VK_CleanupSwapchain();
    if (!VK_CreateSwapchain(width, height)) return false;
    if (!VK_CreateDepthResources())         return false;
    if (!VK_CreateFramebuffers())           return false;
    vk_state.window_width  = width;
    vk_state.window_height = height;
    return true;
}

void VK_ShutdownContext(void)
{
    if (!vk_state.initialized) return;

    vkDeviceWaitIdle(vk_state.device);

    VK_EModel_FreeAll();
    VK_Sprite_FreeAll();
    VK_Model_FreeAll();
    VK_Sky_Shutdown();
    VK_Particles_Shutdown();
    VK_World_Free();
    VK_DrawShutdown();
    VK_ShutdownImages();
    VK_DestroyPipeline3D();
    VK_DestroyPipelineWorld();
    VK_DestroyPipeline2D();

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vk_state.image_available[i])  vkDestroySemaphore(vk_state.device, vk_state.image_available[i],  NULL);
        if (vk_state.render_finished[i])  vkDestroySemaphore(vk_state.device, vk_state.render_finished[i],  NULL);
        if (vk_state.in_flight_fences[i]) vkDestroyFence    (vk_state.device, vk_state.in_flight_fences[i], NULL);
    }
    if (vk_state.command_pool) vkDestroyCommandPool(vk_state.device, vk_state.command_pool, NULL);
    if (vk_state.render_pass)  vkDestroyRenderPass(vk_state.device, vk_state.render_pass, NULL);

    VK_CleanupSwapchain();

    if (vk_state.device) vkDestroyDevice(vk_state.device, NULL);

    if (vk_state.surface) vkDestroySurfaceKHR(vk_state.instance, vk_state.surface, NULL);

    if (vk_state.validation_enabled && vk_state.debug_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT pfn =
            (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(vk_state.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (pfn) pfn(vk_state.instance, vk_state.debug_messenger, NULL);
    }
    if (vk_state.instance) vkDestroyInstance(vk_state.instance, NULL);

    memset(&vk_state, 0, sizeof(vk_state));
}
