#version 450

layout(location = 0) in vec2 frag_texcoord;
layout(location = 1) in vec4 frag_color;
layout(location = 2) in vec3 frag_specular;
layout(location = 3) in float frag_fog;
layout(location = 4) flat in vec4 frag_uv_rect;
layout(location = 5) in vec4 frag_shadow_position;
layout(location = 6) in vec4 frag_shadow_light;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D guest_texture;
// The depth the game's brightest light can see, rendered from where it stands.
layout(set = 1, binding = 2) uniform sampler2D shadow_map;

layout(push_constant) uniform Push {
    mat4 transform;
    vec4 viewport;
    vec4 texture_params; // x: texture enabled, y: texture function, z: alpha ref, w: alpha func
    vec4 uv_transform;
    vec4 view_z;
} push;

// Only the fog colour and the shadow terms are read here. The block is
// described in ge.vert; a uniform block may declare a prefix of the members as
// long as the offsets agree, but the shadow terms sit at the end, so the ones
// in between have to be named to reach them.
layout(set = 1, binding = 0) uniform Environment {
    vec4 ambient;
    vec4 fog;
    vec4 fog_color;
    vec4 light_position[4];
    vec4 light_direction[4];
    vec4 light_attenuation[4];
    vec4 light_spot[4];
    vec4 light_ambient[4];
    vec4 light_diffuse[4];
    vec4 light_specular[4];
    mat4 shadow_transform;
    vec4 shadow_params;
} lighting;

// How much of the shadowing light reaches this fragment: 1 in the open, 0 in
// full shadow. Sampled over a 3x3 block so the map's resolution shows up as a
// soft edge rather than a staircase.
float shadow_reach() {
    if (lighting.shadow_params.x <= 0.0 || frag_shadow_position.w <= 0.0) return 1.0;
    vec3 projected = frag_shadow_position.xyz / frag_shadow_position.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    // Outside the light's box nothing was recorded, so nothing is in shadow.
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    if (projected.z < 0.0 || projected.z > 1.0) return 1.0;
    // Negative marks the debug mode; the size itself is what matters here.
    float texel = abs(lighting.shadow_params.z);
    float bias = lighting.shadow_params.w;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            float recorded = texture(shadow_map, uv + vec2(float(x), float(y)) * texel).r;
            lit += projected.z - bias <= recorded ? 1.0 : 0.0;
        }
    return lit / 9.0;
}

void main() {
    vec4 color = frag_color;
    // Take away the light that something else is standing in front of. This
    // happens before texturing because the lit colour modulates the texture,
    // which is the order the GE shades in.
    // Anything with a place in the light's map can be shadowed. Testing the
    // position rather than the light term matters: on a draw whose lighting is
    // baked there is no term at all, and that is most of the scene.
    if (lighting.shadow_params.x > 0.0 && frag_shadow_position.w > 0.0) {
        float blocked = (1.0 - shadow_reach()) * clamp(lighting.shadow_params.x, 0.0, 1.0);
        if (lighting.shadow_params.z < 0.0) {
            // MGA_SHADOW_DEBUG: paint it red, so a shadow that is working but
            // too subtle to notice cannot be mistaken for one that is not.
            color.rgb = mix(color.rgb, vec3(1.0, 0.0, 0.0), blocked);
        } else if (frag_shadow_light.w > 0.5) {
            // Baked lighting: no term to remove, so the surface is darkened
            // instead. Not to black -- nothing here says how much of its colour
            // came from the blocked light, and a real shadow is still lit by
            // everything else in the scene.
            color.rgb *= 1.0 - blocked * 0.55;
        } else {
            color.rgb = max(color.rgb - frag_shadow_light.rgb * blocked, vec3(0.0));
        }
    }
    if (push.texture_params.x > 0.5) {
        int packed = int(push.texture_params.y + 0.5);
        int function = packed & 7;
        vec2 uv = frag_texcoord;
        // Bit 4: a pixel-mapped 2D draw. Snapping to texel centres makes the
        // smooth sampler return exactly one texel, so the interface stays
        // sharp at any internal resolution without a second sampler.
        if ((packed & 16) != 0) {
            vec2 size = 1.0 / push.uv_transform.xy;
            uv = (floor(uv * size) + 0.5) / size;
        }
        vec4 texel = texture(guest_texture, clamp(uv, frag_uv_rect.xy, frag_uv_rect.zw));
        if (function == 0) {          // modulate
            color *= texel;
        } else if (function == 1) {   // decal
            color = vec4(mix(color.rgb, texel.rgb, texel.a), color.a);
        } else if (function == 2) {   // blend
            color = vec4(mix(color.rgb, texel.rgb, texel.rgb), color.a * texel.a);
        } else {                      // replace and everything else
            color = texel;
        }
        // TFUNC bit 16: the GE doubles the combined colour, clamped. Alpha is
        // left alone.
        if ((packed & 8) != 0) color.rgb = min(color.rgb * 2.0, vec3(1.0));
    }

    // A separate specular term is added after texturing, then fog blends
    // towards its colour; neither touches alpha.
    color.rgb = min(color.rgb + frag_specular, vec3(1.0));
    if ((int(push.viewport.w + 0.5) & 1) != 0) color.rgb = mix(lighting.fog_color.rgb, color.rgb, clamp(frag_fog, 0.0, 1.0));

    // PSP alpha test, evaluated per fragment.
    int alpha_function = int(push.texture_params.w + 0.5);
    float reference = push.texture_params.z / 255.0;
    float alpha = color.a;
    bool passed = true;
    if (alpha_function == 1) passed = false;                     // never
    else if (alpha_function == 2) passed = abs(alpha - reference) < 0.002;
    else if (alpha_function == 3) passed = abs(alpha - reference) >= 0.002;
    else if (alpha_function == 4) passed = alpha < reference;
    else if (alpha_function == 5) passed = alpha <= reference;
    else if (alpha_function == 6) passed = alpha > reference;
    else if (alpha_function == 7) passed = alpha >= reference;
    if (!passed) discard;

    out_color = color;
}
