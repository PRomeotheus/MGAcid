#include "gpu/shadow_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace mga::gpu {
namespace {

// A battle holds a handful of characters. This is generous for that and still
// small enough to keep mapped permanently.
constexpr std::size_t kMaxCasterVertices = 96u * 1024u;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

[[nodiscard]] bool fail(VkResult result, const char *what, std::string &error) {
    if (result == VK_SUCCESS) return false;
    error = what;
    return true;
}

[[nodiscard]] std::array<float, 16> identity() {
    return {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
}

[[nodiscard]] std::array<float, 3> normalise(const std::array<float, 3> &v) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1e-6f)) return {0.0f, 1.0f, 0.0f};
    return {v[0] / length, v[1] / length, v[2] / length};
}

[[nodiscard]] std::array<float, 3> cross(const std::array<float, 3> &a, const std::array<float, 3> &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

[[nodiscard]] float dot(const std::array<float, 3> &a, const std::array<float, 3> &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Rec.709 luminance of a GE colour register (0x00BBGGRR), to pick the light
// that actually lights the scene.
[[nodiscard]] float brightness(std::uint32_t color) {
    const float r = static_cast<float>(color & 0xFFu);
    const float g = static_cast<float>((color >> 8u) & 0xFFu);
    const float b = static_cast<float>((color >> 16u) & 0xFFu);
    return (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f;
}

// A view matrix looking from `eye` along `forward`, column-major, with
// translation in elements 12..14 -- the layout the GE uses and the one the
// renderer's own multiply expects.
[[nodiscard]] std::array<float, 16> look_along(const std::array<float, 3> &eye, const std::array<float, 3> &forward,
                                              const std::array<float, 3> &up_hint) {
    // The camera looks down -z, as OpenGL and the PSP both do.
    const std::array<float, 3> back = normalise({-forward[0], -forward[1], -forward[2]});
    std::array<float, 3> right = cross(up_hint, back);
    if (!(dot(right, right) > 1e-8f)) {
        // The light is straight up or straight down, so the hint is useless.
        right = cross({0.0f, 0.0f, 1.0f}, back);
    }
    right = normalise(right);
    const std::array<float, 3> up = normalise(cross(back, right));
    std::array<float, 16> m = identity();
    m[0] = right[0];  m[4] = right[1];  m[8] = right[2];
    m[1] = up[0];     m[5] = up[1];     m[9] = up[2];
    m[2] = back[0];   m[6] = back[1];   m[10] = back[2];
    m[12] = -dot(right, eye);
    m[13] = -dot(up, eye);
    m[14] = -dot(back, eye);
    return m;
}

// An orthographic box, in the OpenGL convention where clip z runs -w..w. The
// shadow shader applies the same halving the scene's vertices get.
[[nodiscard]] std::array<float, 16> orthographic(float half_width, float half_height, float near_plane,
                                                 float far_plane) {
    const float depth = far_plane - near_plane;
    std::array<float, 16> m{};
    m[0] = 1.0f / std::max(half_width, 1e-3f);
    m[5] = 1.0f / std::max(half_height, 1e-3f);
    m[10] = -2.0f / std::max(depth, 1e-3f);
    m[14] = -(far_plane + near_plane) / std::max(depth, 1e-3f);
    m[15] = 1.0f;
    return m;
}

// Where a shadow should come from when the game will not say. Slightly off
// vertical and off to one side, because a sun directly overhead puts a
// character's shadow entirely under their own feet, where it cannot be seen.
constexpr std::array<float, 3> kSunDirection{0.35f, 1.0f, 0.28f};

// Cofactor expansion. Returns false on a singular matrix rather than dividing
// by zero -- a light left where it was is recoverable, a NaN one is not.
[[nodiscard]] bool invert(const std::array<float, 16> &m, std::array<float, 16> &out) {
    std::array<float, 16> inv{};
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    const float determinant = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-20f) return false;
    const float scale = 1.0f / determinant;
    for (std::size_t i = 0; i < inv.size(); ++i) out[i] = inv[i] * scale;
    return true;
}

[[nodiscard]] std::array<float, 16> multiply(const std::array<float, 16> &a, const std::array<float, 16> &b) {
    std::array<float, 16> result{};
    for (std::uint32_t column = 0; column < 4u; ++column)
        for (std::uint32_t row = 0; row < 4u; ++row) {
            float sum = 0.0f;
            for (std::uint32_t k = 0; k < 4u; ++k) sum += a[k * 4u + row] * b[column * 4u + k];
            result[column * 4u + row] = sum;
        }
    return result;
}

} // namespace

ShadowMap::~ShadowMap() { destroy(); }

std::uint32_t ShadowMap::memory_type(std::uint32_t mask, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties available{};
    vkGetPhysicalDeviceMemoryProperties(context_.physical_device, &available);
    for (std::uint32_t i = 0; i < available.memoryTypeCount; ++i)
        if ((mask & (1u << i)) != 0u && (available.memoryTypes[i].propertyFlags & properties) == properties) return i;
    return 0u;
}

bool ShadowMap::create(const DeviceContext &context, std::uint32_t resolution, const std::uint32_t *vertex_spirv,
                       std::size_t vertex_spirv_bytes, std::string &error) {
    destroy();
    context_ = context;
    resolution_ = std::clamp(resolution, 256u, 4096u);
    if (context_.device == VK_NULL_HANDLE || vertex_spirv == nullptr || vertex_spirv_bytes == 0u) {
        error = "no device or shader for the shadow map";
        return false;
    }
    if (!create_target(error) || !create_pipeline(vertex_spirv, vertex_spirv_bytes, error) || !create_buffer(error)) {
        destroy();
        return false;
    }
    captured_.reserve(kMaxCasterVertices / 8u);
    return true;
}

bool ShadowMap::create_target(std::string &error) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = kDepthFormat;
    image.extent = {resolution_, resolution_, 1u};
    image.mipLevels = 1u;
    image.arrayLayers = 1u;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (fail(vkCreateImage(context_.device, &image, nullptr, &depth_), "vkCreateImage (shadow map)", error))
        return false;

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context_.device, depth_, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (fail(vkAllocateMemory(context_.device, &allocate, nullptr, &depth_memory_), "vkAllocateMemory (shadow map)",
             error))
        return false;
    if (fail(vkBindImageMemory(context_.device, depth_, depth_memory_, 0u), "vkBindImageMemory (shadow map)", error))
        return false;

    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = depth_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = kDepthFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0u, 1u, 0u, 1u};
    if (fail(vkCreateImageView(context_.device, &view, nullptr, &depth_view_), "vkCreateImageView (shadow map)", error))
        return false;

    VkAttachmentDescription attachment{};
    attachment.format = kDepthFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Left ready to sample, which is the only thing anyone does with it.
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference reference{0u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &reference;
    // The map is sampled by the shading that follows it, and was sampled by the
    // shading before it; both edges need naming.
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0u;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0u;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = 1u;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1u;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
    pass.pDependencies = dependencies.data();
    if (fail(vkCreateRenderPass(context_.device, &pass, nullptr, &render_pass_), "vkCreateRenderPass (shadow map)",
             error))
        return false;

    VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer.renderPass = render_pass_;
    framebuffer.attachmentCount = 1u;
    framebuffer.pAttachments = &depth_view_;
    framebuffer.width = resolution_;
    framebuffer.height = resolution_;
    framebuffer.layers = 1u;
    return !fail(vkCreateFramebuffer(context_.device, &framebuffer, nullptr, &framebuffer_),
                 "vkCreateFramebuffer (shadow map)", error);
}

bool ShadowMap::create_pipeline(const std::uint32_t *spirv, std::size_t bytes, std::string &error) {
    VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module.codeSize = bytes;
    module.pCode = spirv;
    if (fail(vkCreateShaderModule(context_.device, &module, nullptr, &vertex_shader_),
             "vkCreateShaderModule (shadow map)", error))
        return false;

    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0u, 64u};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.pushConstantRangeCount = 1u;
    layout.pPushConstantRanges = &range;
    if (fail(vkCreatePipelineLayout(context_.device, &layout, nullptr, &layout_), "vkCreatePipelineLayout (shadow map)",
             error))
        return false;

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vertex_shader_;
    stage.pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0u;
    binding.stride = sizeof(float) * 4u;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attribute{};
    attribute.location = 0u;
    attribute.binding = 0u;
    attribute.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attribute.offset = 0u;
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1u;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 1u;
    vertex_input.pVertexAttributeDescriptions = &attribute;

    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(resolution_), static_cast<float>(resolution_), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {resolution_, resolution_}};
    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1u;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1u;
    viewport_state.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // Both faces write depth. The PSP's winding is not consistent across a
    // CPU-skinned character, and a one-sided shadow caster leaks light.
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    // No colour attachment, so no blend state to describe beyond an empty one.
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 1u;
    info.pStages = &stage;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blending;
    info.layout = layout_;
    info.renderPass = render_pass_;
    return !fail(vkCreateGraphicsPipelines(context_.device, VK_NULL_HANDLE, 1u, &info, nullptr, &pipeline_),
                 "vkCreateGraphicsPipelines (shadow map)", error);
}

bool ShadowMap::create_buffer(std::string &error) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = static_cast<VkDeviceSize>(kMaxCasterVertices) * sizeof(float) * 4u;
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (fail(vkCreateBuffer(context_.device, &info, nullptr, &buffer_), "vkCreateBuffer (shadow map)", error))
        return false;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context_.device, buffer_, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (fail(vkAllocateMemory(context_.device, &allocate, nullptr, &buffer_memory_), "vkAllocateMemory (shadow map)",
             error))
        return false;
    if (fail(vkBindBufferMemory(context_.device, buffer_, buffer_memory_, 0u), "vkBindBufferMemory (shadow map)",
             error))
        return false;
    return !fail(vkMapMemory(context_.device, buffer_memory_, 0u, VK_WHOLE_SIZE, 0u, &mapped_),
                 "vkMapMemory (shadow map)", error);
}

void ShadowMap::destroy() {
    if (context_.device != VK_NULL_HANDLE) {
        if (mapped_ != nullptr) vkUnmapMemory(context_.device, buffer_memory_);
        vkDestroyBuffer(context_.device, buffer_, nullptr);
        vkFreeMemory(context_.device, buffer_memory_, nullptr);
        vkDestroyPipeline(context_.device, pipeline_, nullptr);
        vkDestroyPipelineLayout(context_.device, layout_, nullptr);
        vkDestroyShaderModule(context_.device, vertex_shader_, nullptr);
        vkDestroyFramebuffer(context_.device, framebuffer_, nullptr);
        vkDestroyRenderPass(context_.device, render_pass_, nullptr);
        vkDestroyImageView(context_.device, depth_view_, nullptr);
        vkDestroyImage(context_.device, depth_, nullptr);
        vkFreeMemory(context_.device, depth_memory_, nullptr);
    }
    mapped_ = nullptr;
    buffer_ = VK_NULL_HANDLE;
    buffer_memory_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    vertex_shader_ = VK_NULL_HANDLE;
    framebuffer_ = VK_NULL_HANDLE;
    render_pass_ = VK_NULL_HANDLE;
    depth_view_ = VK_NULL_HANDLE;
    depth_ = VK_NULL_HANDLE;
    depth_memory_ = VK_NULL_HANDLE;
    captured_.clear();
    capture_has_light_ = false;
    previous_valid_ = false;
    // The destructor calls this again, and by then the device may itself be
    // gone. Forgetting it makes the second call do nothing at all.
    context_ = {};
}

void ShadowMap::begin_frame() {
    // This frame's extent becomes next frame's box.
    if (!captured_.empty()) {
        previous_minimum_ = minimum_;
        previous_maximum_ = maximum_;
        previous_valid_ = true;
    }
    captured_.clear();
    capture_has_light_ = false;
    capture_light_ = -1;
    capture_transform_ = identity();
    constexpr float big = std::numeric_limits<float>::max();
    minimum_ = {big, big, big};
    maximum_ = {-big, -big, -big};
}

void ShadowMap::add_casters(const float *positions, std::size_t vertex_count, std::size_t stride,
                           const std::array<float, 16> &world) {
    if (positions == nullptr || stride < 3u) return;
    // Whole triangles only: a partial one would be a stray sliver in the map.
    vertex_count -= vertex_count % 3u;
    // A triangle at a time, so the two ways out below cannot leave a partial one
    // behind. Taking them a vertex at a time was the mistake: hitting the budget
    // or meeting one bad number in the middle of a triangle abandoned the rest of
    // the draw, which took the whole of a character's shadow with it.
    for (std::size_t i = 0; i + 2u < vertex_count; i += 3u) {
        if (captured_.size() + 3u > kMaxCasterVertices) return;
        std::array<std::array<float, 4>, 3> triangle{};
        bool usable = true;
        for (std::size_t corner = 0; corner < 3u && usable; ++corner) {
            const float *p = positions + (i + corner) * stride;
            if (!(std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]))) {
                usable = false;
                break;
            }
            // Column-major, translation in 12..14, as every GE matrix is.
            std::array<float, 4> &at = triangle[corner];
            for (std::size_t row = 0; row < 3u; ++row)
                at[row] = world[row] * p[0] + world[4u + row] * p[1] + world[8u + row] * p[2] + world[12u + row];
            at[3] = 1.0f;
            if (!(std::isfinite(at[0]) && std::isfinite(at[1]) && std::isfinite(at[2]))) usable = false;
        }
        // Skipped, not fatal: one bad triangle costs one triangle.
        if (!usable) continue;
        for (const std::array<float, 4> &at : triangle) {
            captured_.push_back(at);
            for (std::size_t axis = 0; axis < 3u; ++axis) {
                minimum_[axis] = std::min(minimum_[axis], at[axis]);
                maximum_[axis] = std::max(maximum_[axis], at[axis]);
            }
        }
    }
}

bool ShadowMap::resolve_light(const LightingState &lighting, const std::array<float, 16> &draw_to_clip,
                             const std::array<float, 16> &world_to_clip, bool have_world_matrix) {
    // Nothing cast last frame, so there is no box to build and nothing worth
    // casting into it. The first frame of a scene has no shadows.
    if (!previous_valid_) return false;

    // The brightest enabled light wins. A directional light gives its
    // direction in the position slot, which is how the GE reads it and how
    // ge.vert evaluates it; a point or spot light gives a position, and is
    // treated as a direction from that position towards the casters, which is
    // close enough for a shadow when the light is well outside the scene.
    int chosen = -1;
    float best = 0.0f;
    for (std::size_t i = 0; i < lighting.lights.size(); ++i) {
        const LightState &light = lighting.lights[i];
        if (!light.enabled) continue;
        // A light contributing nothing but ambient casts no shadow worth
        // drawing, so the diffuse term is what ranks them.
        const float score = brightness(light.diffuse);
        if (score > best) {
            best = score;
            chosen = static_cast<int>(i);
        }
    }
    // No light with any diffuse colour of its own. That is common when a game
    // bakes its lighting, and it used to mean no shadows at all; instead the
    // light is taken to be roughly overhead, which is where a shadow belongs
    // when nothing says otherwise.
    const bool have_light = chosen >= 0;
    const LightState &light = lighting.lights[static_cast<std::size_t>(std::max(chosen, 0))];

    const std::array<float, 3> centre{(previous_minimum_[0] + previous_maximum_[0]) * 0.5f,
                                      (previous_minimum_[1] + previous_maximum_[1]) * 0.5f,
                                      (previous_minimum_[2] + previous_maximum_[2]) * 0.5f};
    // A sun of the port's own, placed in the world and kept there.
    //
    // The game's light is not usable as a shadow source. Whatever space it is
    // given in turns with the camera, so a shadow cast from it swings round as
    // the view moves -- which reads as wrong immediately, because a shadow that
    // follows your head is the one thing a real shadow never does. Recovering
    // world space costs one matrix inverse, and buys a shadow that stays put.
    //
    // MGA_SHADOW_GAME_LIGHT uses the game's light anyway, for comparison.
    static const bool prefer_game_light = std::getenv("MGA_SHADOW_GAME_LIGHT") != nullptr;
    std::array<float, 3> to_light{};
    world_fixed_ = false;
    std::array<float, 16> draw_to_clip_inverse{};
    if (have_world_matrix && !prefer_game_light && invert(draw_to_clip, draw_to_clip_inverse)) {
        // Both matrices land in the same clip space, so this is world to draw.
        const std::array<float, 16> world_to_draw = multiply(draw_to_clip_inverse, world_to_clip);
        // Only the rotation matters for a direction, and only a rotation is
        // believable: if the recovered matrix is not one, the two matrices did
        // not agree and the result would point somewhere arbitrary.
        const auto column = [&](std::size_t c) {
            return std::array<float, 3>{world_to_draw[c * 4u], world_to_draw[c * 4u + 1u], world_to_draw[c * 4u + 2u]};
        };
        bool rotation = true;
        for (std::size_t c = 0; c < 3u; ++c) {
            const std::array<float, 3> axis = column(c);
            const float length_squared = dot(axis, axis);
            if (!std::isfinite(length_squared) || std::abs(length_squared - 1.0f) > 0.05f) rotation = false;
        }
        if (rotation) {
            const std::array<float, 3> x = column(0), y = column(1), z = column(2);
            to_light = {x[0] * kSunDirection[0] + y[0] * kSunDirection[1] + z[0] * kSunDirection[2],
                        x[1] * kSunDirection[0] + y[1] * kSunDirection[1] + z[1] * kSunDirection[2],
                        x[2] * kSunDirection[0] + y[2] * kSunDirection[1] + z[2] * kSunDirection[2]};
            world_fixed_ = true;
        }
    }
    if (!world_fixed_) {
        // No world matrix, or it did not survive the check. Fall back to the
        // game's light, or to something overhead when it has none.
        to_light = {0.35f, 1.0f, 0.25f};
        if (have_light) {
            to_light = {light.position[0], light.position[1], light.position[2]};
            if (light.type != 0)
                to_light = {to_light[0] - centre[0], to_light[1] - centre[1], to_light[2] - centre[2]};
        }
    }
    to_light = normalise(to_light);
    // A light pointing along nothing, or from directly below the floor, would
    // put the shadow somewhere absurd. Fall back to overhead.
    if (to_light[1] < 0.1f) to_light = {to_light[0] * 0.3f, 1.0f, to_light[2] * 0.3f};
    to_light = normalise(to_light);

    // The box has to hold the casters and the ground they cast onto, so it is
    // sized from their extent with room to spare below and around.
    const std::array<float, 3> extent{previous_maximum_[0] - previous_minimum_[0],
                                      previous_maximum_[1] - previous_minimum_[1],
                                      previous_maximum_[2] - previous_minimum_[2]};
    const float span = std::max({extent[0], extent[1], extent[2], 1000.0f});
    const float half = span * 0.9f + 1200.0f;
    const float distance = half * 3.0f;
    const std::array<float, 3> eye{centre[0] + to_light[0] * distance, centre[1] + to_light[1] * distance,
                                  centre[2] + to_light[2] * distance};
    const std::array<float, 3> forward{-to_light[0], -to_light[1], -to_light[2]};
    const std::array<float, 16> view = look_along(eye, forward, {0.0f, 1.0f, 0.0f});
    const std::array<float, 16> projection = orthographic(half, half, 1.0f, distance + half * 3.0f);
    capture_transform_ = multiply(projection, view);
    light_direction_ = to_light;
    // Kept as it is, including -1: the renderer passes it to the shader, which
    // reads a negative index as "no light of the game's own", and shadows lit
    // geometry by darkening it the way it does unlit geometry.
    capture_light_ = chosen;
    capture_has_light_ = true;
    return true;
}

bool ShadowMap::record(VkCommandBuffer commands) {
    if (!ready() || !capture_has_light_ || captured_.empty() || mapped_ == nullptr) return false;
    const std::size_t count = std::min(captured_.size(), kMaxCasterVertices);
    std::memcpy(mapped_, captured_.data(), count * sizeof(std::array<float, 4>));

    VkClearValue clear{};
    clear.depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffer_;
    pass.renderArea = {{0, 0}, {resolution_, resolution_}};
    pass.clearValueCount = 1u;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdPushConstants(commands, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0u, 64u, capture_transform_.data());
    const VkDeviceSize offset = 0u;
    vkCmdBindVertexBuffers(commands, 0u, 1u, &buffer_, &offset);
    vkCmdDraw(commands, static_cast<std::uint32_t>(count), 1u, 0u, 0u);
    vkCmdEndRenderPass(commands);
    return true;
}

} // namespace mga::gpu
