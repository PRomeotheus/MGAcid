// The GE's registers in a save state.
//
// Apart from ge_state.cpp for the same reason the kernel's is apart from
// kernel.cpp: one file is behaviour, the other is a file format, and a field
// added to a structure in ge_state.hpp has to be added here as well. Keeping
// them separate makes that omission a small readable diff rather than something
// lost in the command decoder.
//
// What is written is everything the decoder would not set again before the next
// draw -- which is most of it, because a PSP game sets its render state up once
// and changes only what varies. What is not written is host wiring (the three
// sinks), scratch reused by every draw (call_), and state that only exists part
// way through a list (call_stack_), since a state is taken between frames.

#include "gpu/ge_state.hpp"

namespace mga::gpu {
namespace {

using psprecomp::SnapshotReader;
using psprecomp::SnapshotWriter;

void write_floats(SnapshotWriter &out, const float *values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) out.f32(values[i]);
}

void read_floats(SnapshotReader &in, float *values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) values[i] = in.f32();
}

void write_texture(SnapshotWriter &out, const TextureState &texture) {
    out.boolean(texture.enabled);
    out.u32(texture.address);
    out.u32(texture.buffer_width);
    out.u16(texture.width);
    out.u16(texture.height);
    out.u32(static_cast<std::uint32_t>(texture.format));
    out.boolean(texture.swizzled);
    out.u32(texture.clut_address);
    out.u32(texture.clut_format);
    out.u32(texture.clut_shift);
    out.u32(texture.clut_mask);
    out.u32(texture.clut_offset);
    out.u32(texture.function);
    out.boolean(texture.alpha_from_texture);
    out.boolean(texture.color_double);
    out.u32(texture.min_filter);
    out.u32(texture.mag_filter);
    out.u32(texture.wrap_s);
    out.u32(texture.wrap_t);
    out.f32(texture.scale_u);
    out.f32(texture.scale_v);
    out.f32(texture.offset_u);
    out.f32(texture.offset_v);
}

void read_texture(SnapshotReader &in, TextureState &texture) {
    texture.enabled = in.boolean();
    texture.address = in.u32();
    texture.buffer_width = in.u32();
    texture.width = in.u16();
    texture.height = in.u16();
    // A format outside the set would reach a switch in the decoder that does
    // not expect it, so it is checked rather than cast.
    const std::uint32_t format = in.u32();
    if (format > static_cast<std::uint32_t>(TextureFormat::Dxt5)) {
        in.fail();
        return;
    }
    texture.format = static_cast<TextureFormat>(format);
    texture.swizzled = in.boolean();
    texture.clut_address = in.u32();
    texture.clut_format = in.u32();
    texture.clut_shift = in.u32();
    texture.clut_mask = in.u32();
    texture.clut_offset = in.u32();
    texture.function = in.u32();
    texture.alpha_from_texture = in.boolean();
    texture.color_double = in.boolean();
    texture.min_filter = in.u32();
    texture.mag_filter = in.u32();
    texture.wrap_s = in.u32();
    texture.wrap_t = in.u32();
    texture.scale_u = in.f32();
    texture.scale_v = in.f32();
    texture.offset_u = in.f32();
    texture.offset_v = in.f32();
}

void write_target(SnapshotWriter &out, const RenderTarget &target) {
    out.u32(target.color_address);
    out.u32(target.color_stride);
    out.u32(target.color_format);
    out.u32(target.depth_address);
    out.u32(target.depth_stride);
}

void read_target(SnapshotReader &in, RenderTarget &target) {
    target.color_address = in.u32();
    target.color_stride = in.u32();
    target.color_format = in.u32();
    target.depth_address = in.u32();
    target.depth_stride = in.u32();
}

void write_blend(SnapshotWriter &out, const BlendState &blend) {
    out.boolean(blend.enabled);
    out.u32(blend.source_factor);
    out.u32(blend.destination_factor);
    out.u32(blend.equation);
    out.u32(blend.fixed_source);
    out.u32(blend.fixed_destination);
}

void read_blend(SnapshotReader &in, BlendState &blend) {
    blend.enabled = in.boolean();
    blend.source_factor = in.u32();
    blend.destination_factor = in.u32();
    blend.equation = in.u32();
    blend.fixed_source = in.u32();
    blend.fixed_destination = in.u32();
}

void write_depth(SnapshotWriter &out, const DepthState &depth) {
    out.boolean(depth.test_enabled);
    out.boolean(depth.write_enabled);
    out.u32(depth.function);
    out.u16(depth.range_near);
    out.u16(depth.range_far);
}

void read_depth(SnapshotReader &in, DepthState &depth) {
    depth.test_enabled = in.boolean();
    depth.write_enabled = in.boolean();
    depth.function = in.u32();
    depth.range_near = in.u16();
    depth.range_far = in.u16();
}

void write_alpha_test(SnapshotWriter &out, const AlphaTestState &alpha) {
    out.boolean(alpha.enabled);
    out.u32(alpha.function);
    out.u32(alpha.reference);
    out.u32(alpha.mask);
}

void read_alpha_test(SnapshotReader &in, AlphaTestState &alpha) {
    alpha.enabled = in.boolean();
    alpha.function = in.u32();
    alpha.reference = in.u32();
    alpha.mask = in.u32();
}

void write_viewport(SnapshotWriter &out, const ViewportState &viewport) {
    out.f32(viewport.x_scale);
    out.f32(viewport.y_scale);
    out.f32(viewport.z_scale);
    out.f32(viewport.x_offset);
    out.f32(viewport.y_offset);
    out.f32(viewport.z_offset);
    out.u32(viewport.scissor_x1);
    out.u32(viewport.scissor_y1);
    out.u32(viewport.scissor_x2);
    out.u32(viewport.scissor_y2);
    out.f32(viewport.offset_x);
    out.f32(viewport.offset_y);
}

void read_viewport(SnapshotReader &in, ViewportState &viewport) {
    viewport.x_scale = in.f32();
    viewport.y_scale = in.f32();
    viewport.z_scale = in.f32();
    viewport.x_offset = in.f32();
    viewport.y_offset = in.f32();
    viewport.z_offset = in.f32();
    viewport.scissor_x1 = in.u32();
    viewport.scissor_y1 = in.u32();
    viewport.scissor_x2 = in.u32();
    viewport.scissor_y2 = in.u32();
    viewport.offset_x = in.f32();
    viewport.offset_y = in.f32();
}

void write_light(SnapshotWriter &out, const LightState &light) {
    out.boolean(light.enabled);
    out.u32(light.kind);
    out.u32(light.type);
    write_floats(out, light.position.data(), light.position.size());
    write_floats(out, light.direction.data(), light.direction.size());
    write_floats(out, light.attenuation.data(), light.attenuation.size());
    out.f32(light.spot_exponent);
    out.f32(light.spot_cutoff);
    out.u32(light.ambient);
    out.u32(light.diffuse);
    out.u32(light.specular);
}

void read_light(SnapshotReader &in, LightState &light) {
    light.enabled = in.boolean();
    light.kind = in.u32();
    light.type = in.u32();
    read_floats(in, light.position.data(), light.position.size());
    read_floats(in, light.direction.data(), light.direction.size());
    read_floats(in, light.attenuation.data(), light.attenuation.size());
    light.spot_exponent = in.f32();
    light.spot_cutoff = in.f32();
    light.ambient = in.u32();
    light.diffuse = in.u32();
    light.specular = in.u32();
}

void write_lighting(SnapshotWriter &out, const LightingState &lighting) {
    out.u32(lighting.material_update);
    out.u32(lighting.material_emissive);
    out.u32(lighting.material_diffuse);
    out.u32(lighting.material_specular);
    out.f32(lighting.specular_power);
    out.u32(lighting.ambient_color);
    out.u32(lighting.ambient_alpha);
    out.u32(lighting.mode);
    out.boolean(lighting.reverse_normals);
    for (const LightState &light : lighting.lights) write_light(out, light);
}

void read_lighting(SnapshotReader &in, LightingState &lighting) {
    lighting.material_update = in.u32();
    lighting.material_emissive = in.u32();
    lighting.material_diffuse = in.u32();
    lighting.material_specular = in.u32();
    lighting.specular_power = in.f32();
    lighting.ambient_color = in.u32();
    lighting.ambient_alpha = in.u32();
    lighting.mode = in.u32();
    lighting.reverse_normals = in.boolean();
    for (LightState &light : lighting.lights) read_light(in, light);
}

void write_fog(SnapshotWriter &out, const FogState &fog) {
    out.boolean(fog.enabled);
    out.f32(fog.end);
    out.f32(fog.scale);
    out.u32(fog.color);
}

void read_fog(SnapshotReader &in, FogState &fog) {
    fog.enabled = in.boolean();
    fog.end = in.f32();
    fog.scale = in.f32();
    fog.color = in.u32();
}

} // namespace

void GeState::write_state(SnapshotWriter &out) const {
    for (const std::uint32_t value : registers_) out.u32(value);
    write_target(out, target_);
    write_texture(out, texture_);
    write_blend(out, blend_);
    write_depth(out, depth_);
    write_alpha_test(out, alpha_test_);
    write_viewport(out, viewport_);
    out.boolean(culling_enabled_);
    out.boolean(cull_clockwise_);
    out.boolean(clear_mode_);
    out.u32(clear_flags_);
    out.u32(material_color_);
    out.boolean(lighting_enabled_);
    write_lighting(out, lighting_);
    write_fog(out, fog_);
    out.u32(vertex_type_);
    out.u32(vertex_address_);
    out.u32(index_address_);
    out.u32(base_extended_);
    out.u32(offset_address_);
    write_floats(out, world_.data(), world_.size());
    write_floats(out, view_.data(), view_.size());
    write_floats(out, projection_.data(), projection_.size());
    write_floats(out, texture_matrix_.data(), texture_matrix_.size());
    write_floats(out, bone_matrices_.data(), bone_matrices_.size());
    out.u32(world_write_index_);
    out.u32(view_write_index_);
    out.u32(projection_write_index_);
    out.u32(texture_write_index_);
    out.u32(bone_write_index_);
    // Not the version counters: they only have to differ from whatever the
    // renderer last saw, and a restore invalidates its bindings anyway, so they
    // carry on from where this session had got to rather than going backwards
    // to a value it has already used.
}

bool GeState::read_state(SnapshotReader &in) {
    for (std::uint32_t &value : registers_) value = in.u32();
    read_target(in, target_);
    read_texture(in, texture_);
    if (!in.ok()) return false;
    read_blend(in, blend_);
    read_depth(in, depth_);
    read_alpha_test(in, alpha_test_);
    read_viewport(in, viewport_);
    culling_enabled_ = in.boolean();
    cull_clockwise_ = in.boolean();
    clear_mode_ = in.boolean();
    clear_flags_ = in.u32();
    material_color_ = in.u32();
    lighting_enabled_ = in.boolean();
    read_lighting(in, lighting_);
    read_fog(in, fog_);
    vertex_type_ = in.u32();
    vertex_address_ = in.u32();
    index_address_ = in.u32();
    base_extended_ = in.u32();
    offset_address_ = in.u32();
    read_floats(in, world_.data(), world_.size());
    read_floats(in, view_.data(), view_.size());
    read_floats(in, projection_.data(), projection_.size());
    read_floats(in, texture_matrix_.data(), texture_matrix_.size());
    read_floats(in, bone_matrices_.data(), bone_matrices_.size());
    world_write_index_ = in.u32();
    view_write_index_ = in.u32();
    projection_write_index_ = in.u32();
    texture_write_index_ = in.u32();
    bone_write_index_ = in.u32();
    // A list was not part way through when the state was written, and must not
    // look as though it were now.
    call_stack_.clear();
    // Every consumer of these has to believe the state changed, because it did.
    ++environment_version_;
    ++material_version_;
    return in.ok();
}

} // namespace mga::gpu
