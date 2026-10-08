// Vulkan renderer. Plain Vulkan 1.0 features only, so it runs on any driver, including software ones.
//
// Each camera frame:
//   camera planes -> staging buffer -> plane images -> [yuv2rgb] -> camera image (RGBA)
//   camera image -> [filter shader] -> picture image (RGBA, camera size)
//   picture image -> [present shader] -> window, with the interface drawn on top
// For a photo the picture image is copied back to memory as RGBA. For a video frame it first goes
// through two small passes that produce NV12 (what video encoders want), which is under half the bytes.
// A capture is collected later, when the GPU has finished, so the main loop never waits for it.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filters.h"
#include "gpu.h"
#include "profile.h"

// The Vulkan library is opened at run time through SDL, so the build needs no libvulkan to link against.
#define VK_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateSampler) \
    X(vkDestroySampler) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) X(vkCreateFence) X(vkDestroyFence) \
    X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus) X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)

#define X(name) static PFN_##name name;
VK_FUNCS(X)
#undef X
static PFN_vkCreateInstance vkCreateInstance;
static PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties;

#define MAX_SWAP_IMAGES 8
#define MAX_FILTERS     64
#define RGBA            VK_FORMAT_R8G8B8A8_UNORM
#define ACQUIRE_WAIT_NS 4000000ull   // how long to wait for the window before drawing without it
#define STAMPS          5

typedef struct {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkFramebuffer  fb;      // only for images we draw into
    VkDeviceSize   bytes;
    int            w, h;
} Image;

typedef struct {
    VkBuffer       buffer;
    VkDeviceMemory memory;
    void          *map;
    VkDeviceSize   bytes;
} Buffer;

// Everything whose size depends on the camera mode. Built as a whole and swapped in only when every
// part exists, so a failed change of mode leaves the working set untouched.
typedef struct {
    CamFormat fmt;
    int       video_w, video_h;       // camera size rounded down to even numbers
    Image     plane[3];               // camera planes as uploaded (unused ones stay empty)
    int       plane_count;
    VkDeviceSize plane_offset[3];     // where each plane sits in the staging buffer
    int       plane_row_bytes[3], plane_rows[3];
    Image     camera;                 // RGBA camera picture the filters read
    Image     picture;                // filtered picture
    Image     video_y, video_uv;      // filtered picture as NV12, for the video encoder
    Buffer    staging;                // camera planes on their way to the GPU
    Buffer    photo_out, video_out;   // captures on their way back
    bool      planes_ready;           // the plane images have been written at least once
} CameraSet;

static struct {
    SDL_Window      *window;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkPhysicalDeviceMemoryProperties mem;
    uint32_t         max_image_side;
    char             device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    VkDevice         dev;
    uint32_t         queue_family;
    VkQueue          queue;
    VkSurfaceKHR     surface;

    VkSwapchainKHR   swapchain;
    VkFormat         swap_format;
    bool             swap_srgb;
    VkExtent2D       swap_extent;
    uint32_t         swap_count;
    VkImageView      swap_views[MAX_SWAP_IMAGES];
    VkFramebuffer    swap_fbs[MAX_SWAP_IMAGES];
    bool             swap_dirty;

    VkRenderPass     pass_rgba, pass_window, pass_y, pass_uv;
    VkSampler        sampler;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipe_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet  set_planes, set_camera, set_picture;
    VkShaderModule   vert;
    VkPipeline       filter_pipes[MAX_FILTERS];
    VkPipeline       present_pipe, yuv_pipe, video_y_pipe, video_uv_pipe;

    CameraSet        cam;
    bool             have_cam;
    bool             upload_pending;       // staging holds a frame not yet copied to the plane images
    int              capture_pending;      // GPU_CAPTURE_* bits submitted and not yet collected
    bool             failed;               // the device is unusable; draw nothing more

    VkCommandPool    cmd_pool;
    VkCommandBuffer  cmd;
    VkFence          fence;
    VkSemaphore      sem_acquired;
    VkSemaphore      sem_rendered[MAX_SWAP_IMAGES];   // one per swapchain image: presentation holds on to it

    // Profiling only: GPU timestamps written between the stages of a frame.
    VkQueryPool      queries;
    float            ns_per_tick;
    bool             queries_pending, queries_upload, queries_filter, queries_capture, queries_window;
    VkDeviceSize     memory_bytes;
} g;

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "picashot: %s failed (%d)\n", #call, (int)r_); return false; } } while (0)

const char *gpu_device_name(void) { return g.device_name; }

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback) {
    for (int pass = 0; pass < 2; pass++) {
        VkMemoryPropertyFlags need = pass == 0 ? want : fallback;
        for (uint32_t i = 0; i < g.mem.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (g.mem.memoryTypes[i].propertyFlags & need) == need) return i;
    }
    return 0;
}

static void image_destroy(Image *im) {
    if (im->fb) vkDestroyFramebuffer(g.dev, im->fb, NULL);
    if (im->view) vkDestroyImageView(g.dev, im->view, NULL);
    if (im->image) vkDestroyImage(g.dev, im->image, NULL);
    if (im->memory) { vkFreeMemory(g.dev, im->memory, NULL); g.memory_bytes -= im->bytes; }
    memset(im, 0, sizeof *im);
}

// Creates an image and its view, plus a framebuffer when `pass` is given (an image we draw into).
static bool image_create(Image *im, int w, int h, VkFormat format, VkImageUsageFlags usage, VkRenderPass pass) {
    im->w = w;
    im->h = h;
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = { (uint32_t)w, (uint32_t)h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    CHECK(vkCreateImage(g.dev, &ci, NULL, &im->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.dev, im->image, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0) };
    CHECK(vkAllocateMemory(g.dev, &ai, NULL, &im->memory));
    im->bytes = req.size;
    g.memory_bytes += req.size;
    CHECK(vkBindImageMemory(g.dev, im->image, im->memory, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CHECK(vkCreateImageView(g.dev, &vi, NULL, &im->view));
    if (pass) {
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
            .attachmentCount = 1, .pAttachments = &im->view, .width = (uint32_t)w, .height = (uint32_t)h, .layers = 1 };
        CHECK(vkCreateFramebuffer(g.dev, &fi, NULL, &im->fb));
    }
    return true;
}

static void buffer_destroy(Buffer *b) {
    if (b->buffer) vkDestroyBuffer(g.dev, b->buffer, NULL);
    if (b->memory) { vkFreeMemory(g.dev, b->memory, NULL); g.memory_bytes -= b->bytes; }   // freeing also unmaps
    memset(b, 0, sizeof *b);
}

static bool buffer_create(Buffer *b, size_t size, VkBufferUsageFlags usage, bool read_often) {
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    CHECK(vkCreateBuffer(g.dev, &ci, NULL, &b->buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.dev, b->buffer, &req);
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    // Reading back from write-combined memory is very slow, so ask for cached memory when we will read it.
    VkMemoryPropertyFlags want = read_often ? host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT : host;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits, want, host) };
    CHECK(vkAllocateMemory(g.dev, &ai, NULL, &b->memory));
    b->bytes = req.size;
    g.memory_bytes += req.size;
    CHECK(vkBindBufferMemory(g.dev, b->buffer, b->memory, 0));
    CHECK(vkMapMemory(g.dev, b->memory, 0, VK_WHOLE_SIZE, 0, &b->map));
    return true;
}

// One colour attachment, cleared at the start. `final_layout` is the layout it is left in.
static bool render_pass_create(VkRenderPass *out, VkFormat format, VkImageLayout final_layout) {
    VkAttachmentDescription att = { .format = format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = final_layout };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ref };
    // Our images are sampled or copied by the frame before and by the passes after, so say so on both sides.
    VkSubpassDependency deps[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
          .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT },
    };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 2, .pDependencies = deps };
    CHECK(vkCreateRenderPass(g.dev, &ci, NULL, out));
    return true;
}

static bool pipeline_create(VkPipeline *out, const uint32_t *frag_spv, size_t frag_bytes, VkRenderPass pass) {
    VkShaderModuleCreateInfo mi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = frag_bytes, .pCode = frag_spv };
    VkShaderModule frag;
    CHECK(vkCreateShaderModule(g.dev, &mi, NULL, &frag));

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = g.vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = frag, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState att = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att };
    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn_states };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vin, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb,
        .pDynamicState = &dyn, .layout = g.pipe_layout, .renderPass = pass };
    VkResult r = vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &ci, NULL, out);
    vkDestroyShaderModule(g.dev, frag, NULL);
    if (r != VK_SUCCESS) { fprintf(stderr, "picashot: could not build a shader pipeline (%d)\n", (int)r); return false; }
    return true;
}

static void swapchain_destroy_views(void) {
    for (uint32_t i = 0; i < g.swap_count; i++) {
        vkDestroyFramebuffer(g.dev, g.swap_fbs[i], NULL);
        vkDestroyImageView(g.dev, g.swap_views[i], NULL);
    }
    g.swap_count = 0;
}

// Builds or rebuilds the swapchain for the current window size. A zero-sized window leaves none.
// `swap_dirty` stays set until this succeeds, so a failure is tried again on the next frame.
static bool swapchain_create(void) {
    vkDeviceWaitIdle(g.dev);
    swapchain_destroy_views();

    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surface, &caps));
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {   // Wayland: the window decides
        int w, h;
        SDL_GetWindowSizeInPixels(g.window, &w, &h);
        extent.width = (uint32_t)SDL_clamp(w, (int)caps.minImageExtent.width, (int)caps.maxImageExtent.width);
        extent.height = (uint32_t)SDL_clamp(h, (int)caps.minImageExtent.height, (int)caps.maxImageExtent.height);
    }
    g.swap_extent = extent;
    if (extent.width == 0 || extent.height == 0) { g.swap_dirty = false; return true; }

    // Mailbox does not make us wait for the display. The main loop paces itself on camera frames, so
    // we do not rely on the swapchain for timing; where only FIFO exists, a busy window is skipped
    // for that frame (see ACQUIRE_WAIT_NS) and captures carry on.
    VkPresentModeKHR modes[16];
    uint32_t mode_count = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surface, &mode_count, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < mode_count; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        for (uint32_t bit = 1; bit <= 8; bit <<= 1)
            if (caps.supportedCompositeAlpha & bit) { alpha = (VkCompositeAlphaFlagBitsKHR)bit; break; }

    VkSwapchainKHR old = g.swapchain;
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g.surface,
        .minImageCount = count, .imageFormat = g.swap_format, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = extent, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = alpha, .presentMode = mode, .clipped = VK_TRUE, .oldSwapchain = old };
    VkResult made = vkCreateSwapchainKHR(g.dev, &ci, NULL, &g.swapchain);
    if (old) vkDestroySwapchainKHR(g.dev, old, NULL);   // retired by the call above whether or not it worked
    if (made != VK_SUCCESS) {
        g.swapchain = VK_NULL_HANDLE;
        fprintf(stderr, "picashot: vkCreateSwapchainKHR failed (%d)\n", (int)made);
        return false;
    }

    VkImage images[MAX_SWAP_IMAGES];
    uint32_t n = 0;
    CHECK(vkGetSwapchainImagesKHR(g.dev, g.swapchain, &n, NULL));
    if (n > MAX_SWAP_IMAGES) { fprintf(stderr, "picashot: the swapchain has too many images (%u)\n", n); return false; }
    CHECK(vkGetSwapchainImagesKHR(g.dev, g.swapchain, &n, images));
    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = g.swap_format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        CHECK(vkCreateImageView(g.dev, &vi, NULL, &g.swap_views[i]));
        VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g.pass_window,
            .attachmentCount = 1, .pAttachments = &g.swap_views[i], .width = extent.width, .height = extent.height,
            .layers = 1 };
        if (vkCreateFramebuffer(g.dev, &fi, NULL, &g.swap_fbs[i]) != VK_SUCCESS) {
            vkDestroyImageView(g.dev, g.swap_views[i], NULL);
            return false;
        }
        g.swap_count = i + 1;
    }
    g.swap_dirty = false;
    return true;
}

// Points a descriptor set's three samplers at up to three images (missing ones repeat the first).
static void point_set_at(VkDescriptorSet set, VkImageView a, VkImageView b, VkImageView c) {
    VkDescriptorImageInfo info[3] = {
        { g.sampler, a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { g.sampler, b ? b : a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { g.sampler, c ? c : a, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkWriteDescriptorSet w[3];
    for (uint32_t i = 0; i < 3; i++)
        w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &info[i] };
    vkUpdateDescriptorSets(g.dev, 3, w, 0, NULL);
}

static void camera_set_destroy(CameraSet *s) {
    for (int i = 0; i < 3; i++) image_destroy(&s->plane[i]);
    image_destroy(&s->camera);
    image_destroy(&s->picture);
    image_destroy(&s->video_y);
    image_destroy(&s->video_uv);
    buffer_destroy(&s->staging);
    buffer_destroy(&s->photo_out);
    buffer_destroy(&s->video_out);
    memset(s, 0, sizeof *s);
}

static bool camera_set_create(CameraSet *s, const CamFormat *fmt) {
    memset(s, 0, sizeof *s);
    s->fmt = *fmt;
    int w = fmt->w, h = fmt->h;
    s->video_w = w & ~1;
    s->video_h = h & ~1;

    // Which plane images the layout needs, and how the planes pack into the staging buffer.
    VkFormat plane_format[3] = { 0 };
    int plane_w[3] = { 0 }, plane_h[3] = { 0 }, bytes_per_texel[3] = { 0 };
    switch (fmt->layout) {
    case CAM_RGBA:   // one plane that goes straight into `camera`
        s->plane_count = 1; plane_w[0] = w; plane_h[0] = h; bytes_per_texel[0] = 4;
        break;
    case CAM_YUYV:
        s->plane_count = 1; plane_format[0] = VK_FORMAT_R8G8_UNORM; plane_w[0] = w; plane_h[0] = h; bytes_per_texel[0] = 2;
        break;
    case CAM_NV12:
        s->plane_count = 2;
        plane_format[0] = VK_FORMAT_R8_UNORM; plane_w[0] = w; plane_h[0] = h; bytes_per_texel[0] = 1;
        plane_format[1] = VK_FORMAT_R8G8_UNORM; plane_w[1] = fmt->chroma_w; plane_h[1] = fmt->chroma_h; bytes_per_texel[1] = 2;
        break;
    case CAM_PLANAR:
        s->plane_count = 3;
        plane_format[0] = VK_FORMAT_R8_UNORM; plane_w[0] = w; plane_h[0] = h; bytes_per_texel[0] = 1;
        for (int i = 1; i < 3; i++) {
            plane_format[i] = VK_FORMAT_R8_UNORM; plane_w[i] = fmt->chroma_w; plane_h[i] = fmt->chroma_h; bytes_per_texel[i] = 1;
        }
        break;
    }
    VkDeviceSize staging_bytes = 0;
    for (int i = 0; i < s->plane_count; i++) {
        if (plane_w[i] < 1 || plane_h[i] < 1 || plane_w[i] > w || plane_h[i] > h) return false;
        s->plane_offset[i] = staging_bytes;
        s->plane_row_bytes[i] = plane_w[i] * bytes_per_texel[i];
        s->plane_rows[i] = plane_h[i];
        staging_bytes += (VkDeviceSize)s->plane_row_bytes[i] * (VkDeviceSize)plane_h[i];
        staging_bytes = (staging_bytes + 3) & ~(VkDeviceSize)3;   // a copy must start on a multiple of four bytes
        if (plane_format[i] && !image_create(&s->plane[i], plane_w[i], plane_h[i], plane_format[i],
                                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_NULL_HANDLE)) return false;
    }

    VkImageUsageFlags drawn = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!image_create(&s->camera, w, h, RGBA, drawn | VK_IMAGE_USAGE_TRANSFER_DST_BIT, g.pass_rgba)) return false;
    if (!image_create(&s->picture, w, h, RGBA, drawn | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, g.pass_rgba)) return false;
    if (!image_create(&s->video_y, s->video_w, s->video_h, VK_FORMAT_R8_UNORM,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, g.pass_y)) return false;
    if (!image_create(&s->video_uv, s->video_w / 2, s->video_h / 2, VK_FORMAT_R8G8_UNORM,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, g.pass_uv)) return false;
    if (!buffer_create(&s->staging, (size_t)staging_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false)) return false;
    if (!buffer_create(&s->photo_out, (size_t)w * (size_t)h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    if (!buffer_create(&s->video_out, (size_t)s->video_w * (size_t)s->video_h * 3 / 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;

    // Black until the first frame arrives. For YUV that is Y = 0 with U = V = 128.
    uint8_t *bytes = s->staging.map;
    memset(bytes, fmt->layout == CAM_RGBA ? 0 : 128, (size_t)staging_bytes);
    if (fmt->layout == CAM_NV12 || fmt->layout == CAM_PLANAR) memset(bytes, 0, (size_t)w * (size_t)h);
    if (fmt->layout == CAM_YUYV) for (size_t i = 0; i < (size_t)w * (size_t)h; i++) bytes[i * 2] = 0;
    return true;
}

bool gpu_set_camera_format(const CamFormat *fmt) {
    if (g.failed) return false;
    if (fmt->w < 2 || fmt->h < 2 || (uint32_t)fmt->w > g.max_image_side || (uint32_t)fmt->h > g.max_image_side ||
        fmt->w > GPU_MAX_CAMERA_SIDE || fmt->h > GPU_MAX_CAMERA_SIDE) {
        fprintf(stderr, "picashot: camera size %dx%d is not usable on this GPU\n", fmt->w, fmt->h);
        return false;
    }
    vkDeviceWaitIdle(g.dev);
    CameraSet fresh;
    if (!camera_set_create(&fresh, fmt)) {
        camera_set_destroy(&fresh);   // the set in use is untouched
        return false;
    }
    if (g.have_cam) camera_set_destroy(&g.cam);
    g.cam = fresh;
    g.have_cam = true;
    g.upload_pending = true;          // draw the black start picture once
    g.capture_pending = 0;
    point_set_at(g.set_planes, g.cam.plane[0].view ? g.cam.plane[0].view : g.cam.camera.view, g.cam.plane[1].view, g.cam.plane[2].view);
    point_set_at(g.set_camera, g.cam.camera.view, VK_NULL_HANDLE, VK_NULL_HANDLE);
    point_set_at(g.set_picture, g.cam.picture.view, VK_NULL_HANDLE, VK_NULL_HANDLE);
    profile_gpu_memory(g.memory_bytes);
    return true;
}

static void wait_for_gpu(void) { vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX); }

bool gpu_upload(const CamFrame *frame) {
    if (g.failed) return false;
    const CamFormat *have = &g.cam.fmt, *got = &frame->fmt;
    if (!g.have_cam || have->w != got->w || have->h != got->h || have->layout != got->layout ||
        have->chroma_w != got->chroma_w || have->chroma_h != got->chroma_h || have->full_range != got->full_range ||
        have->bt709 != got->bt709) {
        if (!gpu_set_camera_format(got)) return false;
    }
    wait_for_gpu();   // the previous frame may still be reading the staging memory
    for (int i = 0; i < g.cam.plane_count; i++) {
        uint8_t *dst = (uint8_t *)g.cam.staging.map + g.cam.plane_offset[i];
        const uint8_t *src = frame->plane[i];
        size_t row = (size_t)g.cam.plane_row_bytes[i];
        int rows = g.cam.plane_rows[i];
        if (!src || frame->pitch[i] < (int)row) return false;
        if ((size_t)frame->pitch[i] == row) memcpy(dst, src, row * (size_t)rows);
        else for (int y = 0; y < rows; y++) memcpy(dst + row * (size_t)y, src + (size_t)frame->pitch[i] * (size_t)y, row);
    }
    g.upload_pending = true;
    return true;
}

void gpu_window_resized(void) { g.swap_dirty = true; }
bool gpu_ok(void) { return !g.failed; }
int  gpu_camera_width(void) { return g.have_cam ? g.cam.fmt.w : 0; }
int  gpu_camera_height(void) { return g.have_cam ? g.cam.fmt.h : 0; }
int  gpu_video_width(void) { return g.have_cam ? g.cam.video_w : 0; }
int  gpu_video_height(void) { return g.have_cam ? g.cam.video_h : 0; }

bool gpu_reload_filter(int index, const uint32_t *spv, size_t bytes) {
    if (g.failed || index < 0 || index >= g_filter_count) return false;
    VkPipeline fresh;
    if (!pipeline_create(&fresh, spv, bytes, g.pass_rgba)) return false;   // keep the old one on failure
    vkDeviceWaitIdle(g.dev);
    vkDestroyPipeline(g.dev, g.filter_pipes[index], NULL);
    g.filter_pipes[index] = fresh;
    return true;
}

static int device_score(VkPhysicalDeviceType type) {
    // A camera app has no need to wake a discrete card, so the integrated one wins when both exist.
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 1;   // software rendering still works
    default:                                     return 0;
    }
}

static bool pick_device(void) {
    VkPhysicalDevice devs[16];
    uint32_t n = 16;
    VkResult r = vkEnumeratePhysicalDevices(g.instance, &n, devs);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) {
        fprintf(stderr, "picashot: no Vulkan device found. Install a Vulkan driver for your GPU, or Mesa's\n"
                        "software driver (lavapipe) to run without one.\n");
        return false;
    }
    const char *forced = SDL_getenv("PICASHOT_GPU");
    int best = -1, best_score = -1;
    uint32_t best_family = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        VkQueueFamilyProperties fams[32];
        uint32_t fam_count = 32;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &fam_count, fams);
        for (uint32_t f = 0; f < fam_count; f++) {
            VkBool32 can_present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], f, g.surface, &can_present);
            if (!(fams[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !can_present) continue;
            int score = device_score(props.deviceType);
            if (forced && (int)i == atoi(forced)) score = 100;
            if (score > best_score) { best = (int)i; best_score = score; best_family = f; }
            break;
        }
    }
    if (best < 0) { fprintf(stderr, "picashot: no Vulkan device can draw to this window\n"); return false; }
    g.phys = devs[best];
    g.queue_family = best_family;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.phys, &props);
    SDL_strlcpy(g.device_name, props.deviceName, sizeof g.device_name);
    g.max_image_side = props.limits.maxImageDimension2D;
    VkQueueFamilyProperties fams[32];
    uint32_t fam_count = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &fam_count, fams);
    g.ns_per_tick = g.queue_family < fam_count && fams[g.queue_family].timestampValidBits ? props.limits.timestampPeriod : 0.0f;
    vkGetPhysicalDeviceMemoryProperties(g.phys, &g.mem);
    return true;
}

bool gpu_init(SDL_Window *window) {
    g.window = window;
    PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!get) { fprintf(stderr, "picashot: Vulkan is not available: %s\n", SDL_GetError()); return false; }
    vkCreateInstance = (PFN_vkCreateInstance)get(NULL, "vkCreateInstance");
    vkEnumerateInstanceLayerProperties =
        (PFN_vkEnumerateInstanceLayerProperties)get(NULL, "vkEnumerateInstanceLayerProperties");
    if (!vkCreateInstance || !vkEnumerateInstanceLayerProperties) {
        fprintf(stderr, "picashot: the Vulkan library is incomplete\n");
        return false;
    }

    Uint32 ext_count = 0;
    const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    // PICASHOT_VALIDATE=1 turns on the Khronos validation layer when it is installed (for development).
    const char *layer = "VK_LAYER_KHRONOS_validation";
    uint32_t layer_count = 0;
    if (SDL_getenv("PICASHOT_VALIDATE")) {
        VkLayerProperties layers[64];
        uint32_t n = 64;
        vkEnumerateInstanceLayerProperties(&n, layers);
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(layers[i].layerName, layer)) layer_count = 1;
        if (!layer_count) fprintf(stderr, "picashot: validation layer not installed, continuing without it\n");
    }
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Picashot",
        .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .enabledExtensionCount = ext_count, .ppEnabledExtensionNames = exts,
        .enabledLayerCount = layer_count, .ppEnabledLayerNames = &layer };
    CHECK(vkCreateInstance(&ici, NULL, &g.instance));
#define X(name) name = (PFN_##name)get(g.instance, #name); \
    if (!name) { fprintf(stderr, "picashot: the Vulkan driver is missing %s\n", #name); return false; }
    VK_FUNCS(X)
#undef X

    if (!SDL_Vulkan_CreateSurface(window, g.instance, NULL, &g.surface)) {
        fprintf(stderr, "picashot: could not create a Vulkan surface: %s\n", SDL_GetError());
        return false;
    }
    if (!pick_device()) return false;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g.queue_family, .queueCount = 1, .pQueuePriorities = &priority };
    const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dev_exts };
    CHECK(vkCreateDevice(g.phys, &dci, NULL, &g.dev));
    vkGetDeviceQueue(g.dev, g.queue_family, 0, &g.queue);

    // Camera bytes are already sRGB-encoded and filters work on them as they are, so a plain UNORM
    // surface shows them unchanged. If the driver only offers sRGB surfaces the present shader compensates.
    VkSurfaceFormatKHR formats[64];
    uint32_t format_count = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surface, &format_count, formats);
    if (format_count == 0) { fprintf(stderr, "picashot: the window offers no surface formats\n"); return false; }
    g.swap_format = formats[0].format;
    for (uint32_t i = 0; i < format_count; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            g.swap_format = formats[i].format;
            break;
        }
    g.swap_srgb = g.swap_format == VK_FORMAT_B8G8R8A8_SRGB || g.swap_format == VK_FORMAT_R8G8B8A8_SRGB;

    if (!render_pass_create(&g.pass_rgba, RGBA, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) return false;
    if (!render_pass_create(&g.pass_window, g.swap_format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)) return false;
    if (!render_pass_create(&g.pass_y, VK_FORMAT_R8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) return false;
    if (!render_pass_create(&g.pass_uv, VK_FORMAT_R8G8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) return false;

    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    CHECK(vkCreateSampler(g.dev, &sci, NULL, &g.sampler));

    // Every shader uses the same layout: three samplers (most use only the first) and 64 bytes of constants.
    VkDescriptorSetLayoutBinding bindings[3];
    for (uint32_t i = 0; i < 3; i++)
        bindings[i] = (VkDescriptorSetLayoutBinding){ .binding = i, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bindings };
    CHECK(vkCreateDescriptorSetLayout(g.dev, &lci, NULL, &g.set_layout));
    VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 * sizeof(float) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
        .pSetLayouts = &g.set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range };
    CHECK(vkCreatePipelineLayout(g.dev, &plci, NULL, &g.pipe_layout));

    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 9 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 3,
        .poolSizeCount = 1, .pPoolSizes = &pool_size };
    CHECK(vkCreateDescriptorPool(g.dev, &pci, NULL, &g.desc_pool));
    VkDescriptorSetLayout layouts[3] = { g.set_layout, g.set_layout, g.set_layout };
    VkDescriptorSet sets[3];
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g.desc_pool, .descriptorSetCount = 3, .pSetLayouts = layouts };
    CHECK(vkAllocateDescriptorSets(g.dev, &dai, sets));
    g.set_planes = sets[0];
    g.set_camera = sets[1];
    g.set_picture = sets[2];

    VkShaderModuleCreateInfo vmi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = g_spv_fullscreen_vert_bytes, .pCode = g_spv_fullscreen_vert };
    CHECK(vkCreateShaderModule(g.dev, &vmi, NULL, &g.vert));
    if (g_filter_count > MAX_FILTERS) { fprintf(stderr, "picashot: too many filters\n"); return false; }
    for (int i = 0; i < g_filter_count; i++)
        if (!pipeline_create(&g.filter_pipes[i], g_filters[i].spv, g_filters[i].spv_bytes, g.pass_rgba)) return false;
    if (!pipeline_create(&g.present_pipe, g_spv_present_frag, g_spv_present_frag_bytes, g.pass_window)) return false;
    if (!pipeline_create(&g.yuv_pipe, g_spv_yuv2rgb_frag, g_spv_yuv2rgb_frag_bytes, g.pass_rgba)) return false;
    if (!pipeline_create(&g.video_y_pipe, g_spv_video_y_frag, g_spv_video_y_frag_bytes, g.pass_y)) return false;
    if (!pipeline_create(&g.video_uv_pipe, g_spv_video_uv_frag, g_spv_video_uv_frag_bytes, g.pass_uv)) return false;

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g.queue_family };
    CHECK(vkCreateCommandPool(g.dev, &cpi, NULL, &g.cmd_pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g.cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    CHECK(vkAllocateCommandBuffers(g.dev, &cai, &g.cmd));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    CHECK(vkCreateFence(g.dev, &fci, NULL, &g.fence));
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    CHECK(vkCreateSemaphore(g.dev, &semi, NULL, &g.sem_acquired));
    for (int i = 0; i < MAX_SWAP_IMAGES; i++) CHECK(vkCreateSemaphore(g.dev, &semi, NULL, &g.sem_rendered[i]));

    if (profile_on && g.ns_per_tick > 0) {
        VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = STAMPS };
        CHECK(vkCreateQueryPool(g.dev, &qi, NULL, &g.queries));
    }
    g.swap_dirty = true;
    return swapchain_create();
}

void gpu_shutdown(void) {
    if (g.dev) {
        vkDeviceWaitIdle(g.dev);
        if (g.have_cam) camera_set_destroy(&g.cam);
        swapchain_destroy_views();
        if (g.swapchain) vkDestroySwapchainKHR(g.dev, g.swapchain, NULL);
        for (int i = 0; i < g_filter_count; i++)
            if (g.filter_pipes[i]) vkDestroyPipeline(g.dev, g.filter_pipes[i], NULL);
        VkPipeline others[] = { g.present_pipe, g.yuv_pipe, g.video_y_pipe, g.video_uv_pipe };
        for (size_t i = 0; i < SDL_arraysize(others); i++)
            if (others[i]) vkDestroyPipeline(g.dev, others[i], NULL);
        if (g.vert) vkDestroyShaderModule(g.dev, g.vert, NULL);
        if (g.sem_acquired) vkDestroySemaphore(g.dev, g.sem_acquired, NULL);
        for (int i = 0; i < MAX_SWAP_IMAGES; i++)
            if (g.sem_rendered[i]) vkDestroySemaphore(g.dev, g.sem_rendered[i], NULL);
        if (g.fence) vkDestroyFence(g.dev, g.fence, NULL);
        if (g.queries) vkDestroyQueryPool(g.dev, g.queries, NULL);
        if (g.cmd_pool) vkDestroyCommandPool(g.dev, g.cmd_pool, NULL);
        if (g.desc_pool) vkDestroyDescriptorPool(g.dev, g.desc_pool, NULL);
        if (g.pipe_layout) vkDestroyPipelineLayout(g.dev, g.pipe_layout, NULL);
        if (g.set_layout) vkDestroyDescriptorSetLayout(g.dev, g.set_layout, NULL);
        if (g.sampler) vkDestroySampler(g.dev, g.sampler, NULL);
        VkRenderPass passes[] = { g.pass_rgba, g.pass_window, g.pass_y, g.pass_uv };
        for (size_t i = 0; i < SDL_arraysize(passes); i++)
            if (passes[i]) vkDestroyRenderPass(g.dev, passes[i], NULL);
        vkDestroyDevice(g.dev, NULL);
    }
    if (g.surface) SDL_Vulkan_DestroySurface(g.instance, g.surface, NULL);
    if (g.instance) vkDestroyInstance(g.instance, NULL);
    memset(&g, 0, sizeof g);
}

static void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src_access,
        .dstAccessMask = dst_access, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(g.cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

// Makes what the GPU copied into a buffer visible to the CPU. Required before reading mapped memory.
static void host_barrier(VkBuffer buffer) {
    VkBufferMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = buffer, .offset = 0, .size = VK_WHOLE_SIZE };
    vkCmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &b, 0, NULL);
}

static void set_viewport(float x, float y, float w, float h) {
    VkViewport vp = { x, y, w, h, 0.0f, 1.0f };
    VkRect2D sc = { { (int32_t)x, (int32_t)y }, { (uint32_t)(w + 0.5f), (uint32_t)(h + 0.5f) } };
    vkCmdSetViewport(g.cmd, 0, 1, &vp);
    vkCmdSetScissor(g.cmd, 0, 1, &sc);
}

// Begins a pass that covers a whole image and binds what the shaders read. With a pipeline it also
// draws the one triangle; without, the caller draws. The caller ends the pass.
static void begin_pass(VkRenderPass pass, const Image *target, VkPipeline pipe, VkDescriptorSet set,
                       const float *constants, uint32_t constant_bytes) {
    VkClearValue clear = { .color = { .float32 = { 0.035f, 0.035f, 0.035f, 1.0f } } };
    VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
        .framebuffer = target->fb, .renderArea = { { 0, 0 }, { (uint32_t)target->w, (uint32_t)target->h } },
        .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(g.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    set_viewport(0, 0, (float)target->w, (float)target->h);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_layout, 0, 1, &set, 0, NULL);
    if (constants) vkCmdPushConstants(g.cmd, g.pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, constant_bytes, constants);
    if (pipe) {
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdDraw(g.cmd, 3, 1, 0, 0);
    }
}

// Profiling: marks the moment the GPU finishes everything recorded so far.
static void stamp(uint32_t index) {
    if (g.queries) vkCmdWriteTimestamp(g.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.queries, index);
}

// Profiling: reads the previous frame's timestamps, once that frame has finished.
static void collect_stamps(void) {
    if (!g.queries || !g.queries_pending) return;
    g.queries_pending = false;
    uint64_t t[STAMPS];
    if (vkGetQueryPoolResults(g.dev, g.queries, 0, STAMPS, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    double ms = (double)g.ns_per_tick / 1e6;
    if (g.queries_upload) profile_ms(PROF_GPU_UPLOAD, (double)(t[1] - t[0]) * ms);
    if (g.queries_filter) profile_ms(PROF_GPU_FILTER, (double)(t[2] - t[1]) * ms);
    if (g.queries_capture) profile_ms(PROF_GPU_READBACK, (double)(t[3] - t[2]) * ms);
    if (g.queries_window) profile_ms(PROF_GPU_PRESENT, (double)(t[4] - t[3]) * ms);
}

// Copies the staging buffer into the plane images (or straight into the camera image for RGBA), then
// converts YUV planes to the RGBA camera image.
static void record_upload(void) {
    CameraSet *s = &g.cam;
    VkImageLayout before = s->planes_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    VkAccessFlags before_access = s->planes_ready ? VK_ACCESS_SHADER_READ_BIT : 0;
    VkPipelineStageFlags before_stage = s->planes_ready ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    for (int i = 0; i < s->plane_count; i++) {
        const Image *im = s->fmt.layout == CAM_RGBA ? &s->camera : &s->plane[i];
        barrier(im->image, before, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, before_access, VK_ACCESS_TRANSFER_WRITE_BIT,
                before_stage, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = { .bufferOffset = s->plane_offset[i], .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { (uint32_t)im->w, (uint32_t)im->h, 1 } };
        vkCmdCopyBufferToImage(g.cmd, s->staging.buffer, im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(im->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    s->planes_ready = true;
    if (s->fmt.layout != CAM_RGBA) {
        float constants[4] = { s->fmt.layout == CAM_NV12 ? 1.0f : s->fmt.layout == CAM_YUYV ? 2.0f : 3.0f,
                               s->fmt.full_range ? 1.0f : 0.0f, s->fmt.bt709 ? 1.0f : 0.0f, 0.0f };
        begin_pass(g.pass_rgba, &s->camera, g.yuv_pipe, g.set_planes, constants, sizeof constants);
        vkCmdEndRenderPass(g.cmd);
    }
}

static void record_filter(const GpuFrame *f) {
    CameraSet *s = &g.cam;
    int n = g_filter_count;
    int cols = f->grid_cols > 0 ? f->grid_cols : 0;
    int rows = cols ? (n + cols - 1) / cols : 0;
    float constants[4] = { (float)s->fmt.w, (float)s->fmt.h, f->time, f->mirror ? 1.0f : 0.0f };
    begin_pass(g.pass_rgba, &s->picture, VK_NULL_HANDLE, g.set_camera, constants, sizeof constants);
    if (cols) {
        // Grid: every filter draws the whole camera picture into its own tile.
        float tw = (float)(s->fmt.w / cols), th = (float)(s->fmt.h / rows);
        for (int i = 0; i < n; i++) {
            set_viewport((float)(i % cols) * tw, (float)(i / cols) * th, tw, th);
            vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.filter_pipes[i]);
            vkCmdDraw(g.cmd, 3, 1, 0, 0);
        }
    } else {
        vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.filter_pipes[f->filter >= 0 && f->filter < n ? f->filter : 0]);
        vkCmdDraw(g.cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(g.cmd);
}

static void record_capture(int capture) {
    CameraSet *s = &g.cam;
    if (capture & GPU_CAPTURE_VIDEO) {
        begin_pass(g.pass_y, &s->video_y, g.video_y_pipe, g.set_picture, NULL, 0);
        vkCmdEndRenderPass(g.cmd);
        begin_pass(g.pass_uv, &s->video_uv, g.video_uv_pipe, g.set_picture, NULL, 0);
        vkCmdEndRenderPass(g.cmd);
        // Both passes leave their image ready to copy from. Y first, then UV: that is NV12.
        VkBufferImageCopy y = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { (uint32_t)s->video_w, (uint32_t)s->video_h, 1 } };
        VkBufferImageCopy uv = { .bufferOffset = (VkDeviceSize)s->video_w * (VkDeviceSize)s->video_h,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { (uint32_t)(s->video_w / 2), (uint32_t)(s->video_h / 2), 1 } };
        vkCmdCopyImageToBuffer(g.cmd, s->video_y.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->video_out.buffer, 1, &y);
        vkCmdCopyImageToBuffer(g.cmd, s->video_uv.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->video_out.buffer, 1, &uv);
        host_barrier(s->video_out.buffer);
    }
    if (capture & GPU_CAPTURE_PHOTO) {
        barrier(s->picture.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { (uint32_t)s->fmt.w, (uint32_t)s->fmt.h, 1 } };
        vkCmdCopyImageToBuffer(g.cmd, s->picture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->photo_out.buffer, 1, &region);
        barrier(s->picture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        host_barrier(s->photo_out.buffer);
    }
}

static void record_window(const GpuFrame *f, uint32_t index) {
    VkClearValue clear = { .color = { .float32 = { 0.035f, 0.035f, 0.035f, 1.0f } } };
    VkRenderPassBeginInfo win = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g.pass_window,
        .framebuffer = g.swap_fbs[index], .renderArea = { { 0, 0 }, g.swap_extent },
        .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(g.cmd, &win, VK_SUBPASS_CONTENTS_INLINE);
    float constants[16] = {
        (float)g.swap_extent.width, (float)g.swap_extent.height, (float)g.cam.fmt.w / (float)g.cam.fmt.h, f->time,
        f->flash, f->countdown, f->rec_seconds, g.swap_srgb ? 1.0f : 0.0f,
        (float)g_filter_count, (float)f->filter, (float)(f->grid_cols > 0 ? f->grid_cols : 0), (float)f->hover,
        (float)f->hover_button, f->sound_on ? 1.0f : 0.0f, f->timer_on ? 1.0f : 0.0f, 0.0f,
    };
    set_viewport(0, 0, (float)g.swap_extent.width, (float)g.swap_extent.height);
    vkCmdBindPipeline(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.present_pipe);
    vkCmdBindDescriptorSets(g.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe_layout, 0, 1, &g.set_picture, 0, NULL);
    vkCmdPushConstants(g.cmd, g.pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, constants);
    vkCmdDraw(g.cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(g.cmd);
}

// After an error that leaves the fence unsignalled, nothing would ever wake a later wait. Replace it.
static void recover_fence(void) {
    vkDestroyFence(g.dev, g.fence, NULL);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    if (vkCreateFence(g.dev, &fci, NULL, &g.fence) != VK_SUCCESS) { g.fence = VK_NULL_HANDLE; g.failed = true; }
}

bool gpu_frame(const GpuFrame *f, bool refilter, int capture) {
    if (g.failed || !g.have_cam) return false;
    wait_for_gpu();
    collect_stamps();
    if (g.capture_pending) return false;              // the caller must collect the last capture first
    if (g.swap_dirty) swapchain_create();              // on failure it stays dirty and we draw without the window

    bool upload = g.upload_pending;
    refilter = refilter || upload || capture;
    // A hidden or busy window must not hold up a recording, so wait only briefly for it.
    bool to_window = !g.swap_dirty && g.swap_extent.width && g.swap_extent.height && g.swap_count;
    uint32_t index = 0;
    if (to_window) {
        VkResult r = vkAcquireNextImageKHR(g.dev, g.swapchain, ACQUIRE_WAIT_NS, g.sem_acquired, VK_NULL_HANDLE, &index);
        if (r == VK_TIMEOUT || r == VK_NOT_READY) to_window = false;
        else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { g.swap_dirty = true; to_window = false; }
    }
    if (!to_window && !capture && !upload) return true;   // nothing to do this time

    vkResetCommandBuffer(g.cmd, 0);
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (vkBeginCommandBuffer(g.cmd, &begin) != VK_SUCCESS) { g.failed = true; return false; }
    if (g.queries) vkCmdResetQueryPool(g.cmd, g.queries, 0, STAMPS);
    stamp(0);
    if (upload) record_upload();
    stamp(1);
    if (refilter) record_filter(f);
    stamp(2);
    if (capture) record_capture(capture);
    stamp(3);
    if (to_window) record_window(f, index);
    stamp(4);
    if (vkEndCommandBuffer(g.cmd) != VK_SUCCESS) { g.failed = true; return false; }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g.cmd };
    if (to_window) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &g.sem_acquired;
        submit.pWaitDstStageMask = &wait_stage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &g.sem_rendered[index];
    }
    vkResetFences(g.dev, 1, &g.fence);
    VkResult submitted = vkQueueSubmit(g.queue, 1, &submit, g.fence);
    if (submitted != VK_SUCCESS) {
        fprintf(stderr, "picashot: vkQueueSubmit failed (%d)\n", (int)submitted);
        recover_fence();
        if (submitted == VK_ERROR_DEVICE_LOST) g.failed = true;
        return false;
    }
    g.upload_pending = false;
    g.capture_pending = capture;
    g.queries_pending = g.queries != VK_NULL_HANDLE;
    g.queries_upload = upload;
    g.queries_filter = refilter;
    g.queries_capture = capture != 0;
    g.queries_window = to_window;
    if (to_window) {
        VkPresentInfoKHR present = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
            .pWaitSemaphores = &g.sem_rendered[index], .swapchainCount = 1, .pSwapchains = &g.swapchain, .pImageIndices = &index };
        VkResult r = vkQueuePresentKHR(g.queue, &present);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g.swap_dirty = true;
    }
    return true;
}

int gpu_capture_pending(void) { return g.capture_pending; }

bool gpu_collect(bool wait, const uint8_t **photo_rgba, const uint8_t **video_nv12) {
    *photo_rgba = *video_nv12 = NULL;
    if (!g.capture_pending || g.failed) return false;
    if (wait) {
        if (vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) { g.failed = true; return false; }
    } else if (vkGetFenceStatus(g.dev, g.fence) != VK_SUCCESS) {
        return false;
    }
    if (g.capture_pending & GPU_CAPTURE_PHOTO) *photo_rgba = g.cam.photo_out.map;
    if (g.capture_pending & GPU_CAPTURE_VIDEO) *video_nv12 = g.cam.video_out.map;
    g.capture_pending = 0;
    return true;
}
