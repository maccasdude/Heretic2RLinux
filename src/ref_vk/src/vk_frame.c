//
// vk_frame.c - Per-frame command recording, clear, and present.
//
// Session 1: just clear to a solid colour each frame. No drawing yet.
//

#include "vk_local.h"
#include "vk_draw.h"
#include "vk_image.h"
#include <string.h>

// Two attachments: clear colour and clear depth. The colour pick survives
// from Session 1 - a dark blue to make it obvious we're in the Vulkan path.
static const VkClearValue k_clear_values[2] = {
    { .color        = { .float32 = { 0.05f, 0.05f, 0.12f, 1.0f } } },
    { .depthStencil = { 1.0f, 0 } },
};

void VK_BeginFrame_impl(float camera_separation)
{
    (void)camera_separation;
    if (!vk_state.initialized) return;

    // Re-bake textures if the gamma/brightness/contrast sliders changed. Runs
    // before any frame work; only does anything on an actual change.
    VK_GammaRefreshIfNeeded();

    const uint32_t frame = vk_state.current_frame;

    // Wait for the previous in-flight frame at this slot to finish.
    vkWaitForFences(vk_state.device, 1, &vk_state.in_flight_fences[frame], VK_TRUE, UINT64_MAX);

    // Acquire the next image from the swapchain.
    VkResult acq = vkAcquireNextImageKHR(
        vk_state.device, vk_state.swapchain, UINT64_MAX,
        vk_state.image_available[frame], VK_NULL_HANDLE,
        &vk_state.current_image_index);

    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        // Window was resized; rebuild swapchain and skip this frame.
        int w, h;
        SDL_GetWindowSize(vk_state.window, &w, &h);
        VK_RecreateSwapchain(w, h);
        return;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        ri.Con_Printf(PRINT_ALL, "vk: vkAcquireNextImageKHR failed (%d)\n", acq);
        return;
    }

    // Only reset the fence once we know we'll submit work.
    vkResetFences(vk_state.device, 1, &vk_state.in_flight_fences[frame]);

    // Start recording the command buffer for this frame.
    VkCommandBuffer cb = vk_state.command_buffers[frame];
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cb, &bi);

    VkRenderPassBeginInfo rpb = {0};
    rpb.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass        = vk_state.render_pass;
    rpb.framebuffer       = vk_state.swapchain_framebuffers[vk_state.current_image_index];
    rpb.renderArea.offset.x = 0;
    rpb.renderArea.offset.y = 0;
    rpb.renderArea.extent = vk_state.swapchain_extent;
    rpb.clearValueCount   = 2;
    rpb.pClearValues      = k_clear_values;
    vkCmdBeginRenderPass(cb, &rpb, VK_SUBPASS_CONTENTS_INLINE);

    // Set up the 2D state (bind pipeline + per-frame vertex buffer + viewport).
    VK_Draw_BeginFrame();

    vk_state.frame_started = true;
}

void VK_EndFrame_impl(void)
{
    if (!vk_state.initialized) return;
    if (!vk_state.frame_started) return;

    const uint32_t frame = vk_state.current_frame;
    VkCommandBuffer cb = vk_state.command_buffers[frame];

    // Flush any pending 2D batch before closing the renderpass.
    VK_Draw_EndFrame();

    vkCmdEndRenderPass(cb);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
        ri.Con_Printf(PRINT_ALL, "vk: vkEndCommandBuffer failed\n");
        vk_state.frame_started = false;
        return;
    }

    VkPipelineStageFlags wait_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {0};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &vk_state.image_available[frame];
    si.pWaitDstStageMask    = &wait_stages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &vk_state.render_finished[frame];

    if (vkQueueSubmit(vk_state.graphics_queue, 1, &si, vk_state.in_flight_fences[frame]) != VK_SUCCESS) {
        ri.Con_Printf(PRINT_ALL, "vk: vkQueueSubmit failed\n");
        vk_state.frame_started = false;
        return;
    }

    VkPresentInfoKHR pi = {0};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &vk_state.render_finished[frame];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &vk_state.swapchain;
    pi.pImageIndices      = &vk_state.current_image_index;

    VkResult pres = vkQueuePresentKHR(vk_state.present_queue, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        int w, h;
        SDL_GetWindowSize(vk_state.window, &w, &h);
        VK_RecreateSwapchain(w, h);
    } else if (pres != VK_SUCCESS) {
        ri.Con_Printf(PRINT_ALL, "vk: vkQueuePresentKHR failed (%d)\n", pres);
    }

    vk_state.current_frame = (vk_state.current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
    vk_state.frame_started = false;
}
