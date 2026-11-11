/* A minimal Vulkan renderer for CUI.
 *
 * The core emits one triangle list, a few draw commands and a texture atlas,
 * so this is: one graphics pipeline, one shader, a sampler per texture, and
 * host-visible vertex/index buffers uploaded every frame. Resizing rebuilds
 * the swapchain. No validation layers, no staging ring, no descriptor
 * indexing: a scaffold to grow from.
 */

#include "cui_vulkan.h"
#include <vulkan/vulkan.h>
#include <SDL2/SDL_vulkan.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_spv.h"

#define VK_MAX_SWAP_IMAGES 8

typedef struct vk_tex {
    VkImage image;
    VkImageView view;
    VkDeviceMemory mem;
    VkDescriptorSet set;
    int w, h, bpp;
    bool uploaded;
} vk_tex;

struct cui_vk {
    SDL_Window *win;
    VkInstance inst;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;

    VkSwapchainKHR swap;
    VkFormat fmt;
    VkExtent2D extent;
    uint32_t nimg;
    VkImage images[VK_MAX_SWAP_IMAGES];
    VkImageView views[VK_MAX_SWAP_IMAGES];
    VkFramebuffer fbs[VK_MAX_SWAP_IMAGES];

    VkFormat rp_fmt;
    VkRenderPass rp;
    VkPipelineLayout pl;
    VkPipeline pipe;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    VkSampler sampler;

    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkSemaphore sem_acquire, sem_done;
    VkFence fence;

    VkBuffer vbuf, ibuf;
    VkDeviceMemory vmem, imem;
    void *vmap, *imap;
    VkDeviceSize vcap, icap;

    vk_tex **tex;
    int tex_cap;

    bool dirty_swap;
};

/* ---- small helpers ---- */

static bool vk__ok(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return true;
    fprintf(stderr, "vulkan: %s failed (%d)\n", what, (int)r);
    return false;
}

static uint32_t vk__mem_type(cui_vk *vk, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return 0;
}

static bool vk__buffer(cui_vk *vk, VkDeviceSize size, VkBufferUsageFlags usage,
                       VkBuffer *buf, VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (!vk__ok(vkCreateBuffer(vk->dev, &ci, NULL, buf), "vkCreateBuffer")) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk->dev, *buf, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = vk__mem_type(vk, req.memoryTypeBits,
                                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    if (!vk__ok(vkAllocateMemory(vk->dev, &ai, NULL, mem), "vkAllocateMemory")) return false;
    vkBindBufferMemory(vk->dev, *buf, *mem, 0);
    if (map && !vk__ok(vkMapMemory(vk->dev, *mem, 0, VK_WHOLE_SIZE, 0, map), "vkMapMemory")) return false;
    return true;
}

static VkCommandBuffer vk__begin_once(cui_vk *vk) {
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = vk->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1 };
    VkCommandBuffer c = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vk->dev, &ai, &c) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(c, &bi);
    return c;
}

static void vk__end_once(cui_vk *vk, VkCommandBuffer c) {
    vkEndCommandBuffer(c);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c };
    vkQueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk->queue);
    vkFreeCommandBuffers(vk->dev, vk->pool, 1, &c);
}

static void vk__image_barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to,
                              VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage,
                              VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                               .srcAccessMask = src, .dstAccessMask = dst,
                               .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(c, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ---- textures ---- */

static bool vk__upload(cui_vk *vk, vk_tex *t, const void *pixels, int x, int y, int w, int h, int stride) {
    if (!pixels || w <= 0 || h <= 0) return true;
    VkDeviceSize bytes = (VkDeviceSize)stride * (VkDeviceSize)h;
    VkBuffer sbuf; VkDeviceMemory smem; void *map = NULL;
    if (!vk__buffer(vk, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sbuf, &smem, &map)) return false;
    memcpy(map, pixels, (size_t)bytes);

    VkCommandBuffer c = vk__begin_once(vk);
    if (!c) return false;
    VkImageLayout from = t->uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    VkAccessFlags from_access = t->uploaded ? VK_ACCESS_SHADER_READ_BIT : 0;
    VkPipelineStageFlags from_stage = t->uploaded ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    vk__image_barrier(c, t->image, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      from_access, VK_ACCESS_TRANSFER_WRITE_BIT,
                      from_stage, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = { .bufferOffset = 0,
                                 .bufferRowLength = (uint32_t)(stride / t->bpp),
                                 .bufferImageHeight = (uint32_t)h,
                                 .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageOffset = { x, y, 0 },
                                 .imageExtent = { (uint32_t)w, (uint32_t)h, 1 } };
    vkCmdCopyBufferToImage(c, sbuf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    vk__image_barrier(c, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vk__end_once(vk, c);

    vkUnmapMemory(vk->dev, smem);
    vkDestroyBuffer(vk->dev, sbuf, NULL);
    vkFreeMemory(vk->dev, smem, NULL);
    t->uploaded = true;
    return true;
}

static bool vk__make_texture(cui_vk *vk, vk_tex *t, int w, int h, cui_texture_format fmt, const void *pixels) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    t->w = w; t->h = h;
    t->bpp = fmt == CUI_TEXTURE_R8 ? 1 : 4;
    VkFormat vf = fmt == CUI_TEXTURE_R8 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = vf, .extent = { (uint32_t)w, (uint32_t)h, 1 }, .mipLevels = 1,
                             .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (!vk__ok(vkCreateImage(vk->dev, &ci, NULL, &t->image), "vkCreateImage")) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->dev, t->image, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                .memoryTypeIndex = vk__mem_type(vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (!vk__ok(vkAllocateMemory(vk->dev, &ai, NULL, &t->mem), "image memory")) return false;
    vkBindImageMemory(vk->dev, t->image, t->mem, 0);

    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = vf,
                                 .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (!vk__ok(vkCreateImageView(vk->dev, &vi, NULL, &t->view), "vkCreateImageView")) return false;

    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = vk->dpool, .descriptorSetCount = 1,
                                         .pSetLayouts = &vk->dsl };
    if (!vk__ok(vkAllocateDescriptorSets(vk->dev, &dsai, &t->set), "vkAllocateDescriptorSets")) return false;
    VkDescriptorImageInfo ii = { .sampler = vk->sampler, .imageView = t->view,
                                 .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = t->set,
                                .dstBinding = 0, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .pImageInfo = &ii };
    vkUpdateDescriptorSets(vk->dev, 1, &wr, 0, NULL);

    if (pixels) {
        int stride = w * t->bpp;
        if (!vk__upload(vk, t, pixels, 0, 0, w, h, stride)) return false;
    }
    return true;
}

static void vk__free_texture(cui_vk *vk, vk_tex *t) {
    if (!t) return;
    if (t->set) vkFreeDescriptorSets(vk->dev, vk->dpool, 1, &t->set);
    if (t->view) vkDestroyImageView(vk->dev, t->view, NULL);
    if (t->image) vkDestroyImage(vk->dev, t->image, NULL);
    if (t->mem) vkFreeMemory(vk->dev, t->mem, NULL);
    free(t);
}

static cui_texture vk__tex_create(void *user, int w, int h, cui_texture_format fmt, const void *pixels) {
    cui_vk *vk = user;
    int id = 1;
    while (id < vk->tex_cap && vk->tex[id]) id++;
    if (id >= vk->tex_cap) {
        int cap = vk->tex_cap ? vk->tex_cap * 2 : 16;
        vk_tex **grown = realloc(vk->tex, sizeof(vk_tex *) * (size_t)cap);
        if (!grown) return 0;
        memset(grown + vk->tex_cap, 0, sizeof(vk_tex *) * (size_t)(cap - vk->tex_cap));
        vk->tex = grown;
        vk->tex_cap = cap;
    }
    vk_tex *t = calloc(1, sizeof *t);
    if (!t) return 0;
    if (!vk__make_texture(vk, t, w, h, fmt, pixels)) { vk__free_texture(vk, t); return 0; }
    vk->tex[id] = t;
    return (cui_texture)id;
}

static void vk__tex_update(void *user, cui_texture tex, int x, int y, int w, int h, const void *pixels, int stride) {
    cui_vk *vk = user;
    if (tex == 0 || tex >= (cui_texture)vk->tex_cap || !vk->tex[tex]) return;
    vk__upload(vk, vk->tex[tex], pixels, x, y, w, h, stride);
}

static void vk__tex_destroy(void *user, cui_texture tex) {
    cui_vk *vk = user;
    if (tex == 0 || tex >= (cui_texture)vk->tex_cap || !vk->tex[tex]) return;
    vk__free_texture(vk, vk->tex[tex]);
    vk->tex[tex] = NULL;
}

cui_renderer_iface cui_vk_iface(cui_vk *vk) {
    cui_renderer_iface iface = { vk, vk__tex_create, vk__tex_update, vk__tex_destroy };
    return iface;
}

/* ---- swapchain, render pass, pipeline ---- */

static bool vk__make_swapchain(cui_vk *vk, VkSwapchainKHR old) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult cr = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->phys, vk->surface, &caps);
    if (cr != VK_SUCCESS) {
        fprintf(stderr, "vulkan: surface capabilities failed (%d)\n", (int)cr);
        return false;
    }

    uint32_t nfmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->phys, vk->surface, &nfmt, NULL);
    VkSurfaceFormatKHR fmts[32];
    if (nfmt > 32) nfmt = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->phys, vk->surface, &nfmt, fmts);
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < nfmt; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            pick = fmts[i];
            break;
        }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(vk->win, &w, &h);
        ext.width = (uint32_t)w;
        ext.height = (uint32_t)h;
    }
    if (ext.width < 1 || ext.height < 1) return false;

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && count > caps.maxImageCount) count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                    .surface = vk->surface, .minImageCount = count, .imageFormat = pick.format,
                                    .imageColorSpace = pick.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
                                    .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                    .preTransform = caps.currentTransform,
                                    .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                    .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE,
                                    .oldSwapchain = old };
    VkResult sr = VK_ERROR_INITIALIZATION_FAILED;
    for (int attempt = 0; attempt < 10; attempt++) {
        VkSurfaceCapabilitiesKHR caps2;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->phys, vk->surface, &caps2) == VK_SUCCESS &&
            caps2.currentExtent.width != UINT32_MAX && caps2.currentExtent.width > 0) {
            ci.imageExtent = caps2.currentExtent;
            ext = caps2.currentExtent;
        }
        sr = vkCreateSwapchainKHR(vk->dev, &ci, NULL, &vk->swap);
        if (sr == VK_SUCCESS) break;
        SDL_Delay(20);
    }
    if (sr != VK_SUCCESS) {
        fprintf(stderr, "vulkan: swapchain %ux%u fmt %d count %u (min %u max %u, alpha 0x%x, transform 0x%x, extent %u..%u x %u..%u) failed %d\n",
                ext.width, ext.height, (int)pick.format, count, caps.minImageCount, caps.maxImageCount,
                caps.supportedCompositeAlpha, caps.currentTransform,
                caps.minImageExtent.width, caps.maxImageExtent.width,
                caps.minImageExtent.height, caps.maxImageExtent.height, (int)sr);
        return false;
    }
    vk->fmt = pick.format;
    vk->extent = ext;

    if (!vk__ok(vkGetSwapchainImagesKHR(vk->dev, vk->swap, &vk->nimg, NULL), "swapchain images")) return false;
    if (vk->nimg > VK_MAX_SWAP_IMAGES) vk->nimg = VK_MAX_SWAP_IMAGES;
    vkGetSwapchainImagesKHR(vk->dev, vk->swap, &vk->nimg, vk->images);
    for (uint32_t i = 0; i < vk->nimg; i++) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = vk->images[i],
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = vk->fmt,
                                     .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (!vk__ok(vkCreateImageView(vk->dev, &vi, NULL, &vk->views[i]), "swapchain view")) return false;
    }
    return true;
}

static bool vk__make_render_pass(cui_vk *vk) {
    VkAttachmentDescription att = { .format = vk->fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkSubpassDependency dep = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
                                .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                  .attachmentCount = 1, .pAttachments = &att,
                                  .subpassCount = 1, .pSubpasses = &sub,
                                  .dependencyCount = 1, .pDependencies = &dep };
    if (!vk__ok(vkCreateRenderPass(vk->dev, &ci, NULL, &vk->rp), "vkCreateRenderPass")) return false;
    vk->rp_fmt = vk->fmt;
    return true;
}

static bool vk__make_pipeline(cui_vk *vk) {
    VkShaderModule vs, fs;
    VkShaderModuleCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = ui_vert_spv_len, .pCode = (const uint32_t *)ui_vert_spv };
    if (!vk__ok(vkCreateShaderModule(vk->dev, &sci, NULL, &vs), "vertex shader")) return false;
    sci.codeSize = ui_frag_spv_len;
    sci.pCode = (const uint32_t *)ui_frag_spv;
    if (!vk__ok(vkCreateShaderModule(vk->dev, &sci, NULL, &fs), "fragment shader")) {
        vkDestroyShaderModule(vk->dev, vs, NULL);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkVertexInputBindingDescription bind = { 0, sizeof(cui_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[8] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(cui_vertex, x) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(cui_vertex, u) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(cui_vertex, lx) },
        { 3, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(cui_vertex, hw) },
        { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(cui_vertex, r_tl) },
        { 5, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(cui_vertex, border) },
        { 6, 0, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)offsetof(cui_vertex, color) },
        { 7, 0, VK_FORMAT_R8G8B8A8_UNORM, (uint32_t)offsetof(cui_vertex, border_color) },
    };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                 .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind,
                                                 .vertexAttributeDescriptionCount = 8, .pVertexAttributeDescriptions = attrs };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                  .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { .blendEnable = VK_TRUE,
                                               .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                               .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                               .colorBlendOp = VK_BLEND_OP_ADD,
                                               .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                               .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                               .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .stageCount = 2, .pStages = stages,
                                         .pVertexInputState = &vin, .pInputAssemblyState = &ia,
                                         .pViewportState = &vp, .pRasterizationState = &rs,
                                         .pMultisampleState = &ms, .pColorBlendState = &cb,
                                         .pDynamicState = &ds, .layout = vk->pl, .renderPass = vk->rp,
                                         .subpass = 0 };
    VkResult r = vkCreateGraphicsPipelines(vk->dev, VK_NULL_HANDLE, 1, &pci, NULL, &vk->pipe);
    vkDestroyShaderModule(vk->dev, vs, NULL);
    vkDestroyShaderModule(vk->dev, fs, NULL);
    return vk__ok(r, "vkCreateGraphicsPipelines");
}

static bool vk__make_framebuffers(cui_vk *vk) {
    for (uint32_t i = 0; i < vk->nimg; i++) {
        VkImageView attachments[1] = { vk->views[i] };
        VkFramebufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = vk->rp,
                                       .attachmentCount = 1, .pAttachments = attachments,
                                       .width = vk->extent.width, .height = vk->extent.height, .layers = 1 };
        if (!vk__ok(vkCreateFramebuffer(vk->dev, &ci, NULL, &vk->fbs[i]), "vkCreateFramebuffer")) return false;
    }
    return true;
}

static void vk__destroy_swapchain(cui_vk *vk) {
    for (uint32_t i = 0; i < vk->nimg; i++) {
        if (vk->fbs[i]) vkDestroyFramebuffer(vk->dev, vk->fbs[i], NULL);
        if (vk->views[i]) vkDestroyImageView(vk->dev, vk->views[i], NULL);
        vk->fbs[i] = VK_NULL_HANDLE;
        vk->views[i] = VK_NULL_HANDLE;
    }
    vk->nimg = 0;
}

static bool vk__recreate(cui_vk *vk) {
    vkDeviceWaitIdle(vk->dev);
    vk__destroy_swapchain(vk);
    VkSwapchainKHR old = vk->swap;
    vk->swap = VK_NULL_HANDLE;
    if (!vk__make_swapchain(vk, old)) return false;
    if (old) vkDestroySwapchainKHR(vk->dev, old, NULL);

    if (vk->rp == VK_NULL_HANDLE || vk->rp_fmt != vk->fmt) {
        if (vk->pipe) { vkDestroyPipeline(vk->dev, vk->pipe, NULL); vk->pipe = VK_NULL_HANDLE; }
        if (vk->rp) { vkDestroyRenderPass(vk->dev, vk->rp, NULL); vk->rp = VK_NULL_HANDLE; }
        if (!vk__make_render_pass(vk) || !vk__make_pipeline(vk)) return false;
    }
    return vk__make_framebuffers(vk);
}

/* ---- public API ---- */

cui_vk *cui_vk_create(SDL_Window *window) {
    cui_vk *vk = calloc(1, sizeof *vk);
    if (!vk) return NULL;
    vk->win = window;

    unsigned next = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &next, NULL)) {
        fprintf(stderr, "vulkan: SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
        goto fail;
    }
    const char **exts = calloc(next, sizeof(char *));
    if (!exts || !SDL_Vulkan_GetInstanceExtensions(window, &next, exts)) goto fail;

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "cui-vulkan",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                 .enabledExtensionCount = next, .ppEnabledExtensionNames = exts };
    VkResult r = vkCreateInstance(&ici, NULL, &vk->inst);
    free(exts);
    if (!vk__ok(r, "vkCreateInstance")) goto fail;

    if (!SDL_Vulkan_CreateSurface(window, vk->inst, &vk->surface)) {
        fprintf(stderr, "vulkan: SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
        goto fail;
    }

    uint32_t nphys = 0;
    vkEnumeratePhysicalDevices(vk->inst, &nphys, NULL);
    if (nphys == 0) { fprintf(stderr, "vulkan: no Vulkan device\n"); goto fail; }
    VkPhysicalDevice *phys = calloc(nphys, sizeof *phys);
    vkEnumeratePhysicalDevices(vk->inst, &nphys, phys);
    vk->phys = VK_NULL_HANDLE;
    vk->qfam = UINT32_MAX;
    int best_score = -1;
    for (uint32_t i = 0; i < nphys; i++) {
        VkPhysicalDeviceProperties dp;
        vkGetPhysicalDeviceProperties(phys[i], &dp);
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &nq, NULL);
        VkQueueFamilyProperties *qs = calloc(nq, sizeof *qs);
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &nq, qs);
        uint32_t gq = UINT32_MAX;
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(phys[i], q, vk->surface, &present);
            if ((qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) gq = q;
        }
        /* The display is usually driven by one GPU; a device that cannot
         * present fails swapchain creation later. Prefer discrete, then
         * integrated, and allow CUI_VK_DEVICE=<index> to override. */
        int score = gq == UINT32_MAX ? -1
                  : dp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                  : dp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (getenv("CUI_VK_DEBUG"))
            fprintf(stderr, "vulkan: device %u: %s (graphics+present queue: %s)\n", i, dp.deviceName,
                    gq == UINT32_MAX ? "no" : "yes");
        free(qs);
        if (score > best_score) { best_score = score; vk->phys = phys[i]; vk->qfam = gq; }
    }
    const char *want = getenv("CUI_VK_DEVICE");
    if (want) {
        uint32_t idx = (uint32_t)atoi(want);
        if (idx < nphys) {
            uint32_t nq = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(phys[idx], &nq, NULL);
            VkQueueFamilyProperties *qs = calloc(nq, sizeof *qs);
            vkGetPhysicalDeviceQueueFamilyProperties(phys[idx], &nq, qs);
            for (uint32_t q = 0; q < nq; q++) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(phys[idx], q, vk->surface, &present);
                if ((qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    vk->phys = phys[idx];
                    vk->qfam = q;
                    break;
                }
            }
            free(qs);
        }
    }
    free(phys);
    if (vk->phys == VK_NULL_HANDLE) { fprintf(stderr, "vulkan: no graphics+present queue\n"); goto fail; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk->phys, &props);
    if (getenv("CUI_VK_DEBUG"))
        fprintf(stderr, "vulkan: device %s (driver %u)\n", props.deviceName, props.driverVersion);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = vk->qfam, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dexts[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dexts };
    if (!vk__ok(vkCreateDevice(vk->phys, &dci, NULL, &vk->dev), "vkCreateDevice")) goto fail;
    vkGetDeviceQueue(vk->dev, vk->qfam, 0, &vk->queue);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vk->qfam };
    if (!vk__ok(vkCreateCommandPool(vk->dev, &pci, NULL, &vk->pool), "command pool")) goto fail;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = vk->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    if (!vk__ok(vkAllocateCommandBuffers(vk->dev, &cai, &vk->cmd), "command buffer")) goto fail;

    VkSamplerCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .maxLod = 0.25f };
    if (!vk__ok(vkCreateSampler(vk->dev, &si, NULL, &vk->sampler), "sampler")) goto fail;

    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                       VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dsl = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 1, .pBindings = &b };
    if (!vk__ok(vkCreateDescriptorSetLayout(vk->dev, &dsl, NULL, &vk->dsl), "descriptor layout")) goto fail;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                       .maxSets = 256, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (!vk__ok(vkCreateDescriptorPool(vk->dev, &dpi, NULL, &vk->dpool), "descriptor pool")) goto fail;

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &vk->dsl,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (!vk__ok(vkCreatePipelineLayout(vk->dev, &plci, NULL, &vk->pl), "pipeline layout")) goto fail;

    VkSemaphoreCreateInfo semci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCreateSemaphore(vk->dev, &semci, NULL, &vk->sem_acquire);
    vkCreateSemaphore(vk->dev, &semci, NULL, &vk->sem_done);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                              .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    if (!vk__ok(vkCreateFence(vk->dev, &fci, NULL, &vk->fence), "fence")) goto fail;

    if (!vk__recreate(vk)) goto fail;
    return vk;

fail:
    cui_vk_destroy(vk);
    return NULL;
}

void cui_vk_resize(cui_vk *vk) {
    if (vk) vk->dirty_swap = true;
}

static bool vk__ensure_buffers(cui_vk *vk, VkDeviceSize vbytes, VkDeviceSize ibytes) {
    if (vbytes > vk->vcap) {
        if (vk->vmap) { vkUnmapMemory(vk->dev, vk->vmem); vk->vmap = NULL; }
        if (vk->vbuf) { vkDestroyBuffer(vk->dev, vk->vbuf, NULL); vk->vbuf = VK_NULL_HANDLE; }
        if (vk->vmem) { vkFreeMemory(vk->dev, vk->vmem, NULL); vk->vmem = VK_NULL_HANDLE; }
        VkDeviceSize cap = vbytes < 64 * 1024 ? 64 * 1024 : vbytes;
        if (!vk__buffer(vk, cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vk->vbuf, &vk->vmem, &vk->vmap)) return false;
        vk->vcap = cap;
    }
    if (ibytes > vk->icap) {
        if (vk->imap) { vkUnmapMemory(vk->dev, vk->imem); vk->imap = NULL; }
        if (vk->ibuf) { vkDestroyBuffer(vk->dev, vk->ibuf, NULL); vk->ibuf = VK_NULL_HANDLE; }
        if (vk->imem) { vkFreeMemory(vk->dev, vk->imem, NULL); vk->imem = VK_NULL_HANDLE; }
        VkDeviceSize cap = ibytes < 64 * 1024 ? 64 * 1024 : ibytes;
        if (!vk__buffer(vk, cap, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &vk->ibuf, &vk->imem, &vk->imap)) return false;
        vk->icap = cap;
    }
    return true;
}

bool cui_vk_render(cui_vk *vk, const cui_draw_data *dd) {
    if (!vk || !dd) return false;
    if (vk->dirty_swap) {
        vk->dirty_swap = false;
        if (!vk__recreate(vk)) return false;
    }

    vkWaitForFences(vk->dev, 1, &vk->fence, VK_TRUE, UINT64_MAX);
    uint32_t img = 0;
    VkResult ar = vkAcquireNextImageKHR(vk->dev, vk->swap, UINT64_MAX, vk->sem_acquire, VK_NULL_HANDLE, &img);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        if (!vk__recreate(vk)) return false;
        return false;
    }
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "vulkan: acquire failed (%d)\n", (int)ar);
        return false;
    }

    vkResetFences(vk->dev, 1, &vk->fence);
    vkResetCommandBuffer(vk->cmd, 0);

    if (!vk__ensure_buffers(vk, (VkDeviceSize)dd->vertex_count * sizeof(cui_vertex),
                            (VkDeviceSize)dd->index_count * sizeof(uint32_t)))
        return false;
    if (dd->vertex_count > 0) memcpy(vk->vmap, dd->vertices, (size_t)dd->vertex_count * sizeof(cui_vertex));
    if (dd->index_count > 0) memcpy(vk->imap, dd->indices, (size_t)dd->index_count * sizeof(uint32_t));

    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(vk->cmd, &bi);

    VkClearValue clear = { .color = { { dd->clear_color.r / 255.0f, dd->clear_color.g / 255.0f,
                                        dd->clear_color.b / 255.0f, 1.0f } } };
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = vk->rp,
                                  .framebuffer = vk->fbs[img], .renderArea = { { 0, 0 }, vk->extent },
                                  .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(vk->cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    if (dd->vertex_count > 0 && dd->index_count > 0) {
        VkViewport vp = { 0, 0, (float)vk->extent.width, (float)vk->extent.height, 0.0f, 1.0f };
        VkRect2D full = { { 0, 0 }, vk->extent };
        vkCmdSetViewport(vk->cmd, 0, 1, &vp);
        vkCmdSetScissor(vk->cmd, 0, 1, &full);
        vkCmdBindPipeline(vk->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->pipe);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(vk->cmd, 0, 1, &vk->vbuf, &off);
        vkCmdBindIndexBuffer(vk->cmd, vk->ibuf, 0, VK_INDEX_TYPE_UINT32);
        float pc[2] = { (float)dd->width_px, (float)dd->height_px };
        vkCmdPushConstants(vk->cmd, vk->pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);

        for (int i = 0; i < dd->command_count; i++) {
            const cui_draw_cmd *c = &dd->commands[i];
            int x0 = (int)c->clip.x, y0 = (int)c->clip.y;
            int x1 = x0 + (int)c->clip.w, y1 = y0 + (int)c->clip.h;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > (int)vk->extent.width) x1 = (int)vk->extent.width;
            if (y1 > (int)vk->extent.height) y1 = (int)vk->extent.height;
            if (x1 <= x0 || y1 <= y0) continue;
            VkRect2D sc = { { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
            vkCmdSetScissor(vk->cmd, 0, 1, &sc);

            cui_texture id = c->texture ? c->texture : dd->font_atlas;
            vk_tex *t = (id > 0 && id < (cui_texture)vk->tex_cap && vk->tex[id]) ? vk->tex[id] : NULL;
            if (!t) t = (vk->tex_cap > 1) ? vk->tex[1] : NULL;
            if (!t) continue;
            vkCmdBindDescriptorSets(vk->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->pl, 0, 1, &t->set, 0, NULL);
            vkCmdDrawIndexed(vk->cmd, c->index_count, 1, c->index_offset, 0, 0);
        }
    }
    vkCmdEndRenderPass(vk->cmd);
    vkEndCommandBuffer(vk->cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .waitSemaphoreCount = 1, .pWaitSemaphores = &vk->sem_acquire,
                        .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &vk->cmd,
                        .signalSemaphoreCount = 1, .pSignalSemaphores = &vk->sem_done };
    if (!vk__ok(vkQueueSubmit(vk->queue, 1, &si, vk->fence), "vkQueueSubmit")) return false;

    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                            .waitSemaphoreCount = 1, .pWaitSemaphores = &vk->sem_done,
                            .swapchainCount = 1, .pSwapchains = &vk->swap, .pImageIndices = &img };
    VkResult pr = vkQueuePresentKHR(vk->queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || ar == VK_SUBOPTIMAL_KHR)
        vk->dirty_swap = true;
    return pr == VK_SUCCESS;
}

void cui_vk_destroy(cui_vk *vk) {
    if (!vk) return;
    if (vk->dev) vkDeviceWaitIdle(vk->dev);
    if (vk->dev) {
        for (int i = 0; i < vk->tex_cap; i++)
            if (vk->tex && vk->tex[i]) vk__free_texture(vk, vk->tex[i]);
        free(vk->tex);
        if (vk->vmap) { vkUnmapMemory(vk->dev, vk->vmem); vk->vmap = NULL; }
        if (vk->imap) { vkUnmapMemory(vk->dev, vk->imem); vk->imap = NULL; }
        if (vk->vbuf) vkDestroyBuffer(vk->dev, vk->vbuf, NULL);
        if (vk->ibuf) vkDestroyBuffer(vk->dev, vk->ibuf, NULL);
        if (vk->vmem) vkFreeMemory(vk->dev, vk->vmem, NULL);
        if (vk->imem) vkFreeMemory(vk->dev, vk->imem, NULL);
    }
    if (vk->dev) {
        vk__destroy_swapchain(vk);
        if (vk->swap) vkDestroySwapchainKHR(vk->dev, vk->swap, NULL);
        if (vk->pipe) vkDestroyPipeline(vk->dev, vk->pipe, NULL);
        if (vk->rp) vkDestroyRenderPass(vk->dev, vk->rp, NULL);
        if (vk->pl) vkDestroyPipelineLayout(vk->dev, vk->pl, NULL);
        if (vk->dsl) vkDestroyDescriptorSetLayout(vk->dev, vk->dsl, NULL);
        if (vk->dpool) vkDestroyDescriptorPool(vk->dev, vk->dpool, NULL);
        if (vk->sampler) vkDestroySampler(vk->dev, vk->sampler, NULL);
        if (vk->fence) vkDestroyFence(vk->dev, vk->fence, NULL);
        if (vk->sem_acquire) vkDestroySemaphore(vk->dev, vk->sem_acquire, NULL);
        if (vk->sem_done) vkDestroySemaphore(vk->dev, vk->sem_done, NULL);
        if (vk->pool) vkDestroyCommandPool(vk->dev, vk->pool, NULL);
        vkDestroyDevice(vk->dev, NULL);
    }
    if (vk->surface && vk->inst) vkDestroySurfaceKHR(vk->inst, vk->surface, NULL);
    if (vk->inst) vkDestroyInstance(vk->inst, NULL);
    free(vk);
}
