//
// vk_local.h - Heretic II R Vulkan renderer (Linux port)
//
// Internal definitions shared between the vk_*.c sources.
//

#ifndef VK_LOCAL_H
#define VK_LOCAL_H

// q_Typedef must come BEFORE SDL/Vulkan headers. q_Typedef defines
// `typedef enum { false, true } qboolean;` (when not C++), and SDL/Vulkan
// pull in stdbool.h / stdint.h which #define true and false as macros.
// Including them after qboolean's enum keeps the macro definition active
// for the C standard library headers without breaking the H2 enum.
#include "qcommon/q_Typedef.h"
#include "quake2/src/client/ref.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#define REF_TITLE  "Vulkan"

#ifdef _WIN32
#define REF_DECLSPEC __declspec(dllexport)
#else
#define REF_DECLSPEC __attribute__((visibility("default")))
#endif

// Maximum number of frames whose command buffers can be in flight at once.
// Two is the standard double-buffer; three lets the GPU stay one frame ahead
// of the CPU at the cost of a frame of input latency.
#define MAX_FRAMES_IN_FLIGHT 2

typedef struct {
    VkInstance               instance;
    VkPhysicalDevice         physical_device;
    VkDevice                 device;
    uint32_t                 graphics_queue_family;
    uint32_t                 present_queue_family;
    VkQueue                  graphics_queue;
    VkQueue                  present_queue;
    VkSurfaceKHR             surface;

    VkSwapchainKHR           swapchain;
    VkFormat                 swapchain_format;
    VkExtent2D               swapchain_extent;
    uint32_t                 swapchain_image_count;
    VkImage*                 swapchain_images;       // [swapchain_image_count]
    VkImageView*             swapchain_image_views;  // [swapchain_image_count]
    VkFramebuffer*           swapchain_framebuffers; // [swapchain_image_count]

    // One depth attachment shared across all swapchain framebuffers - we
    // only render one frame at a time so a single depth image is enough.
    VkFormat                 depth_format;
    VkImage                  depth_image;
    VkDeviceMemory           depth_memory;
    VkImageView              depth_view;

    VkRenderPass             render_pass;
    VkCommandPool            command_pool;
    VkCommandBuffer          command_buffers[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore              image_available[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore              render_finished[MAX_FRAMES_IN_FLIGHT];
    VkFence                  in_flight_fences[MAX_FRAMES_IN_FLIGHT];

    uint32_t                 current_frame;          // index into MAX_FRAMES_IN_FLIGHT
    uint32_t                 current_image_index;    // returned by AcquireNextImage each frame

    qboolean                 initialized;
    qboolean                 frame_started;          // true between BeginFrame and EndFrame

    // Tracks which pipeline state the command buffer is currently set up for,
    // so 2D draws can rebind themselves after R_RenderFrame switches to 3D.
    qboolean                 pipeline_2d_bound;

    SDL_Window*              window;
    int                      window_width;
    int                      window_height;

    qboolean                 validation_enabled;
    VkDebugUtilsMessengerEXT debug_messenger;
} vk_state_t;

extern vk_state_t            vk_state;
extern refimport_t           ri;

// vk_init.c
qboolean VK_InitContext(SDL_Window* window);
void     VK_ShutdownContext(void);
qboolean VK_RecreateSwapchain(int width, int height);

// vk_frame.c (frame begin/present)
void     VK_BeginFrame_impl(float camera_separation);
void     VK_EndFrame_impl(void);

#endif // VK_LOCAL_H
