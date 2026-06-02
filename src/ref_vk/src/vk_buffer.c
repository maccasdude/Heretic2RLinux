//
// vk_buffer.c - Buffer/image memory helpers and texture upload.
//
// Notes on the approach:
//
//  - We use individual VkDeviceMemory allocations per resource. This is wasteful
//    in real apps (Vulkan recommends sub-allocating from large pools) but for an
//    H2-era game with at most a few thousand textures it's fine and far less
//    code than a real allocator. Can be replaced later if memory pressure
//    becomes a thing.
//
//  - All static textures go through a staging buffer: create host-visible
//    buffer, memcpy pixels in, transition image to TRANSFER_DST, copy, then
//    transition to SHADER_READ. Standard pattern.
//

#include "vk_buffer.h"
#include "vk_local.h"

#include <string.h>

uint32_t VK_FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(vk_state.physical_device, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
    }
    return UINT32_MAX;
}

qboolean VK_CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags props, vk_buffer_t* out)
{
    memset(out, 0, sizeof(*out));

    VkBufferCreateInfo bci = {0};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = size;
    bci.usage       = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(vk_state.device, &bci, NULL, &out->buffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk_state.device, out->buffer, &req);

    VkMemoryAllocateInfo ai = {0};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = VK_FindMemoryType(req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX) {
        ri.Con_Printf(PRINT_ALL, "vk: no memory type with props 0x%x\n", props);
        vkDestroyBuffer(vk_state.device, out->buffer, NULL);
        out->buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkAllocateMemory(vk_state.device, &ai, NULL, &out->memory) != VK_SUCCESS) {
        vkDestroyBuffer(vk_state.device, out->buffer, NULL);
        out->buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(vk_state.device, out->buffer, out->memory, 0);

    out->size   = size;
    out->mapped = NULL;
    return true;
}

void VK_DestroyBuffer(vk_buffer_t* buf)
{
    if (!buf) return;
    if (buf->mapped) {
        vkUnmapMemory(vk_state.device, buf->memory);
        buf->mapped = NULL;
    }
    if (buf->buffer) vkDestroyBuffer(vk_state.device, buf->buffer, NULL);
    if (buf->memory) vkFreeMemory   (vk_state.device, buf->memory, NULL);
    memset(buf, 0, sizeof(*buf));
}

qboolean VK_MapBuffer(vk_buffer_t* buf)
{
    if (buf->mapped) return true;
    return vkMapMemory(vk_state.device, buf->memory, 0, buf->size, 0, &buf->mapped) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// One-shot command buffer
// ---------------------------------------------------------------------------

VkCommandBuffer VK_BeginOneShotCmd(void)
{
    VkCommandBufferAllocateInfo ai = {0};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandPool        = vk_state.command_pool;
    ai.commandBufferCount = 1;

    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(vk_state.device, &ai, &cb);

    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

void VK_EndOneShotCmd(VkCommandBuffer cb)
{
    vkEndCommandBuffer(cb);

    // Use a transient fence rather than vkQueueWaitIdle. Waiting on the whole
    // queue is both slower and risky if this is called while a frame's command
    // buffer is mid-recording (e.g. a texture lazily loaded during a draw):
    // we only need THIS upload to finish, not everything else on the queue.
    VkFenceCreateInfo fci = {0};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(vk_state.device, &fci, NULL, &fence);

    VkSubmitInfo si = {0};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    vkQueueSubmit(vk_state.graphics_queue, 1, &si, fence);
    vkWaitForFences(vk_state.device, 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(vk_state.device, fence, NULL);
    vkFreeCommandBuffers(vk_state.device, vk_state.command_pool, 1, &cb);
}

// ---------------------------------------------------------------------------
// Layout transitions
// ---------------------------------------------------------------------------

static void VK_TransitionImage(VkCommandBuffer cb, VkImage img,
                               VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {0};
    b.sType        = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout    = from;
    b.newLayout    = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image        = img;
    b.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel   = 0;
    b.subresourceRange.levelCount     = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount     = 1;

    VkPipelineStageFlags src_stage = 0, dst_stage = 0;

    if (from == VK_IMAGE_LAYOUT_UNDEFINED && to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (from == VK_IMAGE_LAYOUT_UNDEFINED && to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        // Used for linear-tiled streaming textures: skip the staging dance, go
        // straight to shader-read since we'll memcpy into the image's host
        // memory and the GPU will sample directly.
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        // Catch-all: pipeline barrier with full sync.
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        src_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dst_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

// ---------------------------------------------------------------------------
// RGBA static textures (staging-uploaded, sampled from)
// ---------------------------------------------------------------------------

qboolean VK_CreateTextureRGBA(uint32_t w, uint32_t h, const void* rgba_pixels, vk_texture_t* out)
{
    memset(out, 0, sizeof(*out));
    if (w == 0 || h == 0) return false;

    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    const VkDeviceSize byte_size = (VkDeviceSize)w * (VkDeviceSize)h * 4;

    // Staging buffer.
    vk_buffer_t staging = {0};
    if (!VK_CreateBuffer(byte_size,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &staging))
        return false;
    if (!VK_MapBuffer(&staging)) { VK_DestroyBuffer(&staging); return false; }
    memcpy(staging.mapped, rgba_pixels, (size_t)byte_size);

    // Destination image.
    VkImageCreateInfo ici = {0};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = fmt;
    ici.extent.width  = w;
    ici.extent.height = h;
    ici.extent.depth  = 1;
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(vk_state.device, &ici, NULL, &out->image) != VK_SUCCESS) {
        VK_DestroyBuffer(&staging);
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk_state.device, out->image, &req);

    VkMemoryAllocateInfo ai = {0};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = VK_FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(vk_state.device, &ai, NULL, &out->memory) != VK_SUCCESS) {
        vkDestroyImage(vk_state.device, out->image, NULL);
        VK_DestroyBuffer(&staging);
        memset(out, 0, sizeof(*out));
        return false;
    }
    vkBindImageMemory(vk_state.device, out->image, out->memory, 0);

    // Copy staging -> image and transition to SHADER_READ.
    VkCommandBuffer cb = VK_BeginOneShotCmd();
    VK_TransitionImage(cb, out->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width  = w;
    copy.imageExtent.height = h;
    copy.imageExtent.depth  = 1;
    vkCmdCopyBufferToImage(cb, staging.buffer, out->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VK_TransitionImage(cb, out->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VK_EndOneShotCmd(cb);

    VK_DestroyBuffer(&staging);

    // Image view.
    VkImageViewCreateInfo vci = {0};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = out->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vk_state.device, &vci, NULL, &out->view) != VK_SUCCESS) {
        VK_DestroyTexture(out);
        return false;
    }

    out->width  = w;
    out->height = h;
    out->format = fmt;
    return true;
}

void VK_DestroyTexture(vk_texture_t* tex)
{
    if (!tex) return;
    if (tex->view)   vkDestroyImageView(vk_state.device, tex->view,   NULL);
    if (tex->image)  vkDestroyImage    (vk_state.device, tex->image,  NULL);
    if (tex->memory) vkFreeMemory      (vk_state.device, tex->memory, NULL);
    memset(tex, 0, sizeof(*tex));
}

// Upload a sub-rectangle of RGBA pixels into an existing texture. 'src' points
// at the top-left of the region and is tightly packed at src_stride bytes per
// row (>= region_w*4). Used for per-frame animated-lightstyle atlas updates.
// Synchronous (fenced one-shot); intended to be called once per frame before
// the region is sampled, not in a hot loop.
qboolean VK_UpdateTextureRGBA(vk_texture_t* tex, uint32_t x, uint32_t y,
                              uint32_t region_w, uint32_t region_h,
                              const void* src, uint32_t src_stride)
{
    if (!tex || !tex->image || region_w == 0 || region_h == 0) return false;

    const VkDeviceSize byte_size = (VkDeviceSize)region_w * (VkDeviceSize)region_h * 4;
    vk_buffer_t staging = {0};
    if (!VK_CreateBuffer(byte_size,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &staging))
        return false;
    if (!VK_MapBuffer(&staging)) { VK_DestroyBuffer(&staging); return false; }

    // Pack the region rows tightly into the staging buffer.
    const byte* s = (const byte*)src;
    byte* d = (byte*)staging.mapped;
    for (uint32_t row = 0; row < region_h; row++)
        memcpy(d + (size_t)row * region_w * 4, s + (size_t)row * src_stride, (size_t)region_w * 4);

    VkCommandBuffer cb = VK_BeginOneShotCmd();
    VK_TransitionImage(cb, tex->image,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset.x = (int32_t)x;
    copy.imageOffset.y = (int32_t)y;
    copy.imageExtent.width  = region_w;
    copy.imageExtent.height = region_h;
    copy.imageExtent.depth  = 1;
    vkCmdCopyBufferToImage(cb, staging.buffer, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VK_TransitionImage(cb, tex->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VK_EndOneShotCmd(cb);

    VK_DestroyBuffer(&staging);
    return true;
}

qboolean VK_UpdateTextureRegions(vk_texture_t* tex, const void* src, uint32_t src_w,
                                 const vk_tex_region_t* regions, int num_regions)
{
    if (!tex || !tex->image || !src || num_regions <= 0) return false;

    VkDeviceSize total = 0;
    for (int k = 0; k < num_regions; k++)
        total += (VkDeviceSize)regions[k].w * regions[k].h * 4;
    if (total == 0) return false;

    vk_buffer_t staging = {0};
    if (!VK_CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &staging))
        return false;
    if (!VK_MapBuffer(&staging)) { VK_DestroyBuffer(&staging); return false; }

    VkBufferImageCopy* copies = (VkBufferImageCopy*)calloc(num_regions, sizeof(VkBufferImageCopy));
    if (!copies) { VK_DestroyBuffer(&staging); return false; }

    const byte* srcb = (const byte*)src;
    byte* dstbuf = (byte*)staging.mapped;
    VkDeviceSize off = 0;
    for (int k = 0; k < num_regions; k++) {
        const vk_tex_region_t* r = &regions[k];
        const byte* s = srcb + ((size_t)r->src_y * src_w + r->src_x) * 4;
        for (uint32_t row = 0; row < r->h; row++)
            memcpy(dstbuf + off + (size_t)row * r->w * 4,
                   s + (size_t)row * src_w * 4, (size_t)r->w * 4);
        copies[k].bufferOffset      = off;
        copies[k].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copies[k].imageSubresource.layerCount = 1;
        copies[k].imageOffset.x = (int32_t)r->x;
        copies[k].imageOffset.y = (int32_t)r->y;
        copies[k].imageExtent.width  = r->w;
        copies[k].imageExtent.height = r->h;
        copies[k].imageExtent.depth  = 1;
        off += (VkDeviceSize)r->w * r->h * 4;
    }

    VkCommandBuffer cb = VK_BeginOneShotCmd();
    VK_TransitionImage(cb, tex->image,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cb, staging.buffer, tex->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)num_regions, copies);
    VK_TransitionImage(cb, tex->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VK_EndOneShotCmd(cb);

    free(copies);
    VK_DestroyBuffer(&staging);
    return true;
}


// ---------------------------------------------------------------------------
// Streaming textures (cinematics)
// ---------------------------------------------------------------------------

qboolean VK_CreateStreamingTextureRGBA(uint32_t w, uint32_t h, vk_texture_t* out)
{
    memset(out, 0, sizeof(*out));
    if (w == 0 || h == 0) return false;
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo ici = {0};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = fmt;
    ici.extent.width  = w;
    ici.extent.height = h;
    ici.extent.depth  = 1;
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_LINEAR;     // <- so we can memcpy
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

    if (vkCreateImage(vk_state.device, &ici, NULL, &out->image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk_state.device, out->image, &req);

    VkMemoryAllocateInfo ai = {0};
    ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize  = req.size;
    ai.memoryTypeIndex = VK_FindMemoryType(req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(vk_state.device, &ai, NULL, &out->memory) != VK_SUCCESS) {
        vkDestroyImage(vk_state.device, out->image, NULL);
        memset(out, 0, sizeof(*out));
        return false;
    }
    vkBindImageMemory(vk_state.device, out->image, out->memory, 0);

    // Transition PREINITIALIZED -> SHADER_READ_ONLY_OPTIMAL (skip staging).
    VkCommandBuffer cb = VK_BeginOneShotCmd();
    VK_TransitionImage(cb, out->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VK_EndOneShotCmd(cb);

    VkImageViewCreateInfo vci = {0};
    vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image    = out->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vk_state.device, &vci, NULL, &out->view) != VK_SUCCESS) {
        VK_DestroyTexture(out);
        return false;
    }

    out->width  = w;
    out->height = h;
    out->format = fmt;
    return true;
}

qboolean VK_UpdateStreamingTexture(vk_texture_t* tex, const void* rgba_pixels)
{
    void* mapped = NULL;
    VkSubresourceLayout layout = {0};
    VkImageSubresource sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    vkGetImageSubresourceLayout(vk_state.device, tex->image, &sr, &layout);

    if (vkMapMemory(vk_state.device, tex->memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        return false;

    const uint8_t* src = (const uint8_t*)rgba_pixels;
    uint8_t* dst       = (uint8_t*)mapped + layout.offset;
    const size_t row_bytes = (size_t)tex->width * 4u;
    for (uint32_t y = 0; y < tex->height; y++)
        memcpy(dst + y * layout.rowPitch, src + y * row_bytes, row_bytes);

    vkUnmapMemory(vk_state.device, tex->memory);
    return true;
}
