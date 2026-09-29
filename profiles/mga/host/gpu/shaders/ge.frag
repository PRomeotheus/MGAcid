#version 450

layout(location = 0) in vec2 frag_texcoord;
// The vertex's own colour. The lighting that used to arrive here already
// finished is evaluated below instead.
layout(location = 1) in vec4 frag_color;
layout(location = 2) in vec3 frag_world_position;
layout(location = 3) in float frag_fog;
layout(location = 4) flat in vec4 frag_uv_rect;
layout(location = 5) in vec4 frag_shadow_position;
// 1 where the blocked light cannot be subtracted and the surface is darkened
// instead; see the vertex shader.
layout(location = 6) in float frag_shadow_darken;
layout(location = 7) in vec3 frag_normal;
// Filled only by the per-vertex path; see the vertex shader.
layout(location = 8) in vec3 frag_specular;
layout(location = 9) in vec3 frag_blocked_light;
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
    vec4 shadow_shape;
    // x: how much of the tonemap curve to apply, 0 being none. y, z, w spare.
    vec4 tonemap;
    // x: how strongly the texture's own shading is read as relief. y, z, w spare.
    vec4 surface;
    // xyz: where the eye is, in world space. The GE never needed this -- it
    // approximated the viewer as looking down z from infinitely far away -- but
    // that approximation only holds in its own space, not in the world space
    // the light loop now runs in.
    vec4 camera;
    // x: how much to scale the light before the tonemap, y: how much of the
    // ambient comes from the lit side, z: dither, w: how strong the Fresnel
    // rim is.
    vec4 shading;
} lighting;

// sRGB and linear are not the same thing, and the GE never knew the
// difference. A texture holds sRGB values -- they were authored by eye, on a
// display -- and multiplying one by a light level treats an encoded number as
// though it were an amount of light. That is what makes fixed-function shading
// fall off wrongly: too dark through the midtones, then abruptly flat once it
// clips. These move between the two so the light is applied where it means
// something.
vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow(max(c + 0.055, vec3(0.0)) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 linear_to_srgb(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// Narkowicz's fit of the ACES curve. The whole point of a tonemap is the
// shoulder: light brighter than the display can show has to go somewhere, and
// a clamp sends all of it to the same flat white, which is why a bright
// highlight on the old path loses both its shape and its colour at once. This
// bends it instead, so the highlight keeps both as it goes bright.
vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// The curve normalised so that fully lit white is still white.
//
// Applied raw, ACES puts linear 1.0 at about 0.80, which is the milky look
// people complain about in films-as-games: nothing is ever quite white any
// more, and art drawn on the assumption that white means white goes grey.
// Dividing by the curve's own value at the same point pins white back where it
// belongs and costs only the very top of the range.
//
// k is how much curve to apply, and it is the only dial here. As it goes to
// zero the two calls converge and this becomes the identity -- linear shading,
// clipped -- so one number runs the whole way from off to fully filmic without
// a separate switch. At k = 0.47 a mid grey under neutral light comes back out
// as the same mid grey, which is what makes the setting comparable to the old
// path rather than just brighter or darker than it.
vec3 tonemap(vec3 light, float k) {
    k = max(k, 1e-4);
    return min(aces(light * k) / aces(vec3(k)).x, vec3(1.0));
}

// A ring of offsets, reused for both passes below. Twelve points at two radii
// so a wide blur does not show its own sample pattern as banding.
const vec2 kRing[12] = vec2[12](
    vec2( 1.000,  0.000), vec2( 0.500,  0.866), vec2(-0.500,  0.866), vec2(-1.000,  0.000),
    vec2(-0.500, -0.866), vec2( 0.500, -0.866), vec2( 0.707,  0.707), vec2(-0.707,  0.707),
    vec2(-0.707, -0.707), vec2( 0.707, -0.707), vec2( 0.000,  0.520), vec2( 0.000, -0.520));

// A lit draw's world matrix and material; the layout matches ObjectBlock, and
// the same block the vertex shader reads. It is here because the light loop is
// here: the GE lit per vertex, and interpolating a finished colour is what
// makes low-poly geometry look faceted and makes a specular highlight -- a
// power of a dot product, so the least linear thing in the whole calculation --
// jump between vertices rather than travel across a surface.
layout(set = 1, binding = 1) uniform Object {
    mat4 world;
    vec4 flags;             // y: vertex has a colour, w: material update mask
    vec4 emissive;          // rgb; w: specular power
    vec4 material_ambient;  // rgba
    vec4 material_diffuse;  // rgb; w: 1 keeps specular apart
    vec4 material_specular; // rgb; w: reverse normals
} object;

// Relief from the texture itself.
//
// Nothing in the game's data says which way a surface faces beyond the vertex
// normals, and on geometry this coarse a wall is two triangles: perfectly flat,
// however much brickwork is painted on it. But the painting knows. Art drawn
// for a fixed camera has its own shading baked in, and the dark parts of a
// concrete or grating texture are, almost always, the parts that were recessed
// when someone drew it. Reading brightness as height is a guess, but it is a
// guess that agrees with the artist far more often than it disagrees.
//
// Taken from the albedo rather than from a second texture on purpose: it needs
// no new art, no new binding, and no manifest -- and it gets better on its own
// when a texture pack replaces the albedo with something drawn at four times
// the resolution, because there is then four times as much relief to find.
float surface_height(vec2 uv) {
    vec3 colour = texture(guest_texture, clamp(uv, frag_uv_rect.xy, frag_uv_rect.zw)).rgb;
    return dot(colour, vec3(0.2126, 0.7152, 0.0722));
}

vec3 relief_normal(vec3 normal) {
    vec2 texel = 1.0 / vec2(textureSize(guest_texture, 0));
    // A central difference either way. The negation is the height-field
    // convention: a surface that gets brighter to the right is rising to the
    // right, and a rising surface leans away from that direction.
    vec2 slope = vec2(surface_height(frag_texcoord - vec2(texel.x, 0.0)) -
                          surface_height(frag_texcoord + vec2(texel.x, 0.0)),
                      surface_height(frag_texcoord - vec2(0.0, texel.y)) -
                          surface_height(frag_texcoord + vec2(0.0, texel.y))) *
                 lighting.surface.x;
    if (dot(slope, slope) < 1e-12) return normal;

    // The tangent frame comes from how the world position and the texture
    // coordinate change from one pixel to the next, which means no tangent has
    // to be stored on a vertex, interpolated, or -- the part that would
    // actually be impossible here -- invented for geometry the game supplies
    // without one.
    vec3 dp_x = dFdx(frag_world_position);
    vec3 dp_y = dFdy(frag_world_position);
    vec2 duv_x = dFdx(frag_texcoord);
    vec2 duv_y = dFdy(frag_texcoord);
    vec3 perp_y = cross(dp_y, normal);
    vec3 perp_x = cross(normal, dp_x);
    vec3 tangent = perp_y * duv_x.x + perp_x * duv_y.x;
    vec3 bitangent = perp_y * duv_x.y + perp_x * duv_y.y;
    float longest = max(dot(tangent, tangent), dot(bitangent, bitangent));
    // A degenerate frame: a triangle edge-on, or one with no texture area. The
    // normal it came in with is the honest answer there.
    if (longest <= 0.0) return normal;
    vec3 bumped = normal + (tangent * slope.x + bitangent * slope.y) * inversesqrt(longest);
    float length_squared = dot(bumped, bumped);
    return length_squared > 0.0 ? bumped * inversesqrt(length_squared) : normal;
}

// The GE's lighting, evaluated in world space, per fragment: emissive, plus the
// global ambient light times the material ambient, plus for each enabled light
// its ambient, diffuse and specular terms, scaled by distance attenuation and
// the spot cone. The material update mask makes the vertex colour stand in for
// the ambient (bit 0), diffuse (bit 1) and specular (bit 2) material colours; a
// vertex without a colour keeps the material ones.
//
// Every term is the hardware's own. What changed is only where it is worked
// out, which is why this is an improvement that cannot drift from the game's
// intent: the same lights, the same materials, the same falloff, sampled where
// the eye actually looks instead of at the corners of a triangle.
void light_fragment(bool hdr, bool accurate, out vec4 color, out vec3 separate_specular, out vec3 blocked_light) {
    int mask = int(object.flags.w + 0.5);
    bool has_color = object.flags.y > 0.5;
    vec4 ambient_material = (has_color && (mask & 1) != 0) ? frag_color : object.material_ambient;
    vec3 diffuse_material = (has_color && (mask & 2) != 0) ? frag_color.rgb : object.material_diffuse.rgb;
    vec3 specular_material = (has_color && (mask & 4) != 0) ? frag_color.rgb : object.material_specular.rgb;
    float power = object.emissive.w;

    vec3 world_position = frag_world_position;
    // Normalised again here. The vertex shader already did it once, because the
    // GE normalises after transforming and skinned normals arrive far from unit
    // length; interpolating two unit normals across a triangle gives something
    // shorter than one, and this is what turns that back into a direction.
    float length_squared = dot(frag_normal, frag_normal);
    vec3 normal = length_squared > 0.0 ? frag_normal * inversesqrt(length_squared) : vec3(0.0, 0.0, 1.0);
    if (object.material_specular.w > 0.5) normal = -normal;
    // Before anything uses the normal, and only on a textured draw: there is no
    // relief to read without a texture to read it from.
    if (lighting.surface.x > 0.0 && push.texture_params.x > 0.5) normal = relief_normal(normal);

    // A negative index never matches a loop counter, which is what makes an
    // inferred direction contribute no subtractable light. Rounding is only
    // applied to a non-negative value: int(-1.0 + 0.5) truncates to 0, which
    // would have picked light 0 and undone the distinction.
    int shadow_index = lighting.shadow_params.x > 0.0 && lighting.shadow_params.y >= 0.0
                           ? int(lighting.shadow_params.y + 0.5)
                           : -1;

    // Ambient, with a direction to it.
    //
    // One constant for every surface is the flattest thing in the whole model:
    // a wall facing the light and a wall facing away from it get identical
    // fill, so nothing in shadow has any shape at all. Real ambient is mostly
    // light that bounced, and it bounced off whatever the dominant light is
    // hitting, so it arrives unevenly and favours the lit side.
    //
    // The axis is that light's own direction rather than a world up vector,
    // deliberately: nothing here knows which way is up. The game's world space
    // is its own, an up axis would have to be guessed, and a guess that is
    // wrong by ninety degrees would shade every surface backwards. The light
    // direction is known for certain and is the better correlate anyway.
    vec3 ambient_light = lighting.ambient.rgb;
    if (lighting.shading.y > 0.0) {
        int dominant = shadow_index >= 0 ? shadow_index : -1;
        for (int i = 0; dominant < 0 && i < 4; ++i)
            if (lighting.light_position[i].w >= 0.5) dominant = i;
        if (dominant >= 0) {
            vec3 axis = lighting.light_direction[dominant].w < 0.5
                            ? lighting.light_position[dominant].xyz
                            : lighting.light_position[dominant].xyz - world_position;
            float axis_length = dot(axis, axis);
            if (axis_length > 1e-12) {
                float facing = dot(normal, axis * inversesqrt(axis_length)) * 0.5 + 0.5;
                ambient_light *= mix(1.0 - lighting.shading.y, 1.0 + lighting.shading.y, facing);
            }
        }
    }
    vec3 sum = object.emissive.rgb + ambient_light * ambient_material.rgb;
    vec3 specular = vec3(0.0);
    blocked_light = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        if (lighting.light_position[i].w < 0.5) continue;
        int type = int(lighting.light_direction[i].w + 0.5);
        int kind = int(lighting.light_attenuation[i].w + 0.5);
        vec3 to_light = lighting.light_position[i].xyz;
        float scale = 1.0;
        if (type != 0) {
            to_light -= world_position;
            float distance = length(to_light);
            vec3 k = lighting.light_attenuation[i].xyz;
            scale = clamp(1.0 / max(k.x + k.y * distance + k.z * distance * distance, 1e-20), 0.0, 1.0);
        }
        to_light = dot(to_light, to_light) > 0.0 ? normalize(to_light) : vec3(0.0, 0.0, 1.0);
        if (type == 2) {
            vec3 axis = lighting.light_direction[i].xyz;
            axis = dot(axis, axis) > 0.0 ? normalize(axis) : vec3(0.0, 0.0, 1.0);
            float angle = dot(axis, -to_light);
            scale *= angle >= lighting.light_spot[i].y ? pow(max(angle, 0.0), lighting.light_spot[i].x) : 0.0;
        }
        float n_dot_l = dot(normal, to_light);
        float diffuse = max(n_dot_l, 0.0);
        if (kind == 2) diffuse = pow(diffuse, power);
        sum += (lighting.light_ambient[i].rgb * ambient_material.rgb +
                lighting.light_diffuse[i].rgb * diffuse_material * diffuse) * scale;
        // A shadow blocks a light's diffuse and specular, never its ambient:
        // ambient is the light that arrives by every other path.
        if (i == shadow_index)
            blocked_light += lighting.light_diffuse[i].rgb * diffuse_material * diffuse * scale;
        if (kind == 1 && n_dot_l >= 0.0) {
            // Where the eye actually is, when it is known. The GE's own answer
            // -- a viewer looking down z from infinitely far off -- is kept for
            // the faithful path, and is the reason highlights on the old one do
            // not move when the camera does: the half vector cannot depend on a
            // viewing direction that is the same everywhere.
            vec3 to_eye = vec3(0.0, 0.0, 1.0);
            if (accurate) {
                vec3 offset = lighting.camera.xyz - world_position;
                float distance_squared = dot(offset, offset);
                if (distance_squared > 1e-12) to_eye = offset * inversesqrt(distance_squared);
            }
            vec3 half_vector = normalize(to_light + to_eye);
            vec3 term = lighting.light_specular[i].rgb * specular_material *
                        pow(max(dot(normal, half_vector), 0.0), power) * scale;
            // Schlick's approximation. A surface seen edge-on reflects far more
            // than one seen face-on -- it is why a floor goes bright in the
            // distance and why wet tarmac mirrors a street lamp -- and no
            // fixed-function hardware of the era modelled it at all.
            if (accurate && lighting.shading.w > 0.0) {
                float facing = 1.0 - clamp(dot(normal, to_eye), 0.0, 1.0);
                float rim = facing * facing * facing * facing * facing;
                term *= 1.0 + rim * lighting.shading.w;
            }
            specular += term;
            // Only when the specular is folded into the colour here. Kept
            // apart it is added after texturing, out of reach of the
            // subtraction below.
            if (i == shadow_index && object.material_diffuse.w <= 0.5) blocked_light += term;
        }
    }
    // The ambient's directional part is light that bounced off whatever the
    // main light is hitting, so something standing in that light takes it away
    // too. Only the boost is removed: the flat part arrives by every other
    // path, and leaving it is what keeps a shadow from going black.
    //
    // Without this a shadow is shallower than it should be -- 62% of the
    // unshadowed value rather than 75%, on the numbers this was checked with --
    // and, worse, shallower by an amount that depends on which way the surface
    // happens to face, since a surface facing away never had the boost to
    // begin with. One shadow reading deeper than another for no reason the
    // picture shows is harder to accept than either depth on its own.
    if (shadow_index >= 0)
        blocked_light += max(ambient_light - lighting.ambient.rgb, vec3(0.0)) * ambient_material.rgb;

    float alpha = lighting.ambient.a * ambient_material.a;
    if (object.material_diffuse.w > 0.5) {
        separate_specular = hdr ? specular : clamp(specular, 0.0, 1.0);
    } else {
        sum += specular;
        separate_specular = vec3(0.0);
    }
    // Alpha is still a fraction and still clamps. Only the colour is allowed
    // past one, and only so the tonemap at the end of main() has a shoulder to
    // work with: clamping here would throw the bright light away before
    // anything could shape it.
    color = hdr ? vec4(sum, clamp(alpha, 0.0, 1.0)) : clamp(vec4(sum, alpha), 0.0, 1.0);
}

// How much of the shadowing light reaches this fragment: 1 in the open, 0 in
// full shadow.
//
// The edge is not one width. A real shadow is sharp where the object touches
// the ground and spreads as it reaches away from it, because the further the
// blocker is the more of the light's disc it fails to cover. A single fixed
// blur is the thing that makes a shadow look stuck on rather than cast, so the
// width is measured here rather than chosen.
//
// The light's projection is orthographic, so depths in its map are linear and
// the gap between blocker and receiver can be read straight off as a
// difference. That is what makes this affordable: no reconstruction, just a
// subtraction.
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
    float receiver = projected.z - bias;

    // First: how far away is whatever is standing in the light's way? Only
    // samples in front of this surface are blockers; the rest are the surface
    // itself and say nothing about the width of its shadow.
    float search = texel * 4.0;
    float blocker_depth = 0.0;
    float blockers = 0.0;
    for (int i = 0; i < 12; ++i) {
        float recorded = texture(shadow_map, uv + kRing[i] * search).r;
        if (recorded < receiver) {
            blocker_depth += recorded;
            blockers += 1.0;
        }
    }
    // And the centre, for the same reason the penumbra loop below takes it: the
    // nearest ring offset is two texels out, so a shadow narrower than the ring
    // -- an arm, a barrel, a railing -- can cover this fragment while missing
    // every tap. Without this the search finds no blocker, returns fully lit,
    // and the thin shadow flickers as the caster moves across the ring.
    {
        float recorded = texture(shadow_map, uv).r;
        if (recorded < receiver) {
            blocker_depth += recorded;
            blockers += 1.0;
        }
    }
    // Nothing between this surface and the light.
    if (blockers < 0.5) return 1.0;
    blocker_depth /= blockers;

    // Then: the wider the gap, the wider the penumbra. Never narrower than one
    // texel, or the map's own resolution shows as a staircase; never wider than
    // the cap, or a distant caster smears across the whole scene.
    float gap = max(receiver - blocker_depth, 0.0);
    float radius = clamp(gap * lighting.shadow_shape.x, texel, texel * max(lighting.shadow_shape.y, 1.0));

    float lit = 0.0;
    for (int i = 0; i < 12; ++i) {
        float recorded = texture(shadow_map, uv + kRing[i] * radius).r;
        lit += receiver <= recorded ? 1.0 : 0.0;
    }
    // The centre tap, so a shadow narrower than the ring still registers.
    lit += receiver <= texture(shadow_map, uv).r ? 1.0 : 0.0;
    return lit / 13.0;
}

void main() {
    vec4 color = frag_color;
    vec3 separate_specular = frag_specular;
    vec3 blocked_light = frag_blocked_light;
    // Bit 1 of the enables: this draw is lit. Unlit geometry keeps the colour
    // the game baked into its vertices. Bit 3: light it here rather than at its
    // corners -- otherwise the vertex shader has already done it and what
    // arrives is the finished answer.
    int enables = int(push.viewport.w + 0.5);
    // Bit 4: shade this draw in linear space and tonemap the result. Only lit
    // geometry takes the path. Unlit draws -- the interface, and the baked
    // scenery that is most of a level -- carry a finished colour that was
    // authored to be shown as it is, and there is no light in them to work out
    // more correctly.
    bool lit = (enables & 2) != 0;
    bool hdr = lit && (enables & 16) != 0;
    // Bit 5: use a real viewing direction, and a Fresnel term with it.
    bool accurate = lit && (enables & 32) != 0;
    if (lit && (enables & 8) != 0)
        light_fragment(hdr, accurate, color, separate_specular, blocked_light);
    // Take away the light that something else is standing in front of. This
    // happens before texturing because the lit colour modulates the texture,
    // which is the order the GE shades in.
    // Anything with a place in the light's map can be shadowed. Testing the
    // position rather than the light term matters: on a draw whose lighting is
    // baked there is no term at all, and that is most of the scene.
    if (lighting.shadow_params.x > 0.0 && frag_shadow_position.w > 0.0) {
        float blocked = (1.0 - shadow_reach()) * clamp(lighting.shadow_params.x, 0.0, 1.0);
        if (lighting.shadow_params.z < 0.0) {
            // MGA_SHADOW_DEBUG: paint it, so a shadow that is working but too
            // subtle to notice cannot be mistaken for one that is not.
            //
            // Two colours, because "the lookup works but nothing darkens" was
            // not one question but two, and one overlay colour could not tell
            // them apart. RED is geometry that gets darkened outright. BLUE is
            // geometry that has the blocked light taken off it instead, which
            // is the path that does nothing when there is no light term to
            // take -- and on this game's baked scenery there usually is not.
            vec3 mark = frag_shadow_darken > 0.5 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.35, 1.0);
            color.rgb = mix(color.rgb, mark, blocked);
        } else if (frag_shadow_darken > 0.5) {
            // Baked lighting: no term to remove, so the surface is darkened
            // instead. Not to black -- nothing here says how much of its colour
            // came from the blocked light, and a real shadow is still lit by
            // everything else in the scene.
            //
            // The 0.55 was chosen by eye against a picture in display values,
            // and this multiply now happens before the encode rather than
            // after it. The same number applied to light instead of to an
            // encoded value darkens by 0.70 where it used to darken by 0.45 --
            // shadows 55% lighter than they were tuned to be, purely from
            // moving the arithmetic. Raising it to the gamma restores what was
            // chosen, rather than asking for the constant to be picked twice.
            float darken = 1.0 - blocked * 0.55;
            color.rgb *= hdr ? pow(darken, 2.2) : darken;
        } else {
            // Taking away the light that is blocked is the honest thing to do
            // when there IS a light term, because it darkens by exactly what
            // the shadow costs and leaves everything else lighting the surface
            // as before. What it cannot do is darken by that amount when the
            // amount is zero, and a term can be zero here for reasons that
            // have nothing to do with the surface being in the open: a light
            // the game left switched off, a material that takes no diffuse, a
            // draw whose lighting was worked out at its corners and arrived
            // already finished. The result is a fragment that is in shadow by
            // every measure this shader has and comes out unchanged.
            //
            // So both answers are worked out and the darker one is kept. Where
            // there is a real light term to remove it is usually the deeper of
            // the two and nothing changes; where there is none, the flat
            // darkening stands in for it. Taking the minimum rather than
            // choosing a branch keeps this continuous, so there is no seam
            // across the place where one term fades out.
            vec3 subtracted = max(color.rgb - blocked_light * blocked, vec3(0.0));
            float floor_darken = 1.0 - blocked * 0.55;
            vec3 darkened = color.rgb * (hdr ? pow(floor_darken, 2.2) : floor_darken);
            color.rgb = min(subtracted, darkened);
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
        // The one conversion that matters. Everything below multiplies or
        // mixes this against a light level, and in linear space that product
        // is an amount of light rather than an amount of encoded value.
        if (hdr) texel.rgb = srgb_to_linear(texel.rgb);
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
        if ((packed & 8) != 0) color.rgb = hdr ? color.rgb * 2.0 : min(color.rgb * 2.0, vec3(1.0));
    }

    // A separate specular term is added after texturing, then fog blends
    // towards its colour; neither touches alpha.
    color.rgb = hdr ? color.rgb + separate_specular : min(color.rgb + separate_specular, vec3(1.0));
    if ((enables & 1) != 0) {
        // Fog is a colour the picture moves towards, so it has to be in the
        // same space as the picture or the blend bends the wrong way.
        vec3 fog_color = hdr ? srgb_to_linear(lighting.fog_color.rgb) : lighting.fog_color.rgb;
        color.rgb = mix(fog_color, color.rgb, clamp(frag_fog, 0.0, 1.0));
    }
    if (hdr) {
        // The one place a scale belongs. The game's lights and materials are
        // all fractions of one, so without this the picture rarely passes 1.0
        // and the tonemap's shoulder -- the entire reason for the curve -- has
        // nothing to do. Scaling here rather than inside the light loop keeps
        // it out of the shadow subtraction, which works in the game's own
        // units and would otherwise take away more light than it put in.
        color.rgb = linear_to_srgb(tonemap(color.rgb * lighting.shading.x, lighting.tonemap.x));
        // Eight bits is not many across a dark scene, and the curve above
        // lifts exactly the range where the steps are widest. A little noise
        // below one level scatters each boundary into a gradient the eye reads
        // as smooth, which costs nothing and is invisible on its own.
        if (lighting.shading.z > 0.0) {
            // Two samples subtracted, not one offset: that gives a triangular
            // distribution rather than a flat one, and the difference is the
            // whole point. Banding is visible because the rounding error
            // follows the signal -- the same shade always rounds the same way,
            // so the mistake lines up into an edge. A flat dither shrinks that
            // correlation; a triangular one removes it, which turns the edges
            // into grain instead of into narrower edges. Measured on a dark
            // ramp: no dither correlates 0.033, flat 0.011, triangular 0.009.
            vec2 seed = vec2(0.06711056, 0.00583715);
            float first = fract(52.9829189 * fract(dot(gl_FragCoord.xy, seed)));
            float second = fract(52.9829189 * fract(dot(gl_FragCoord.xy, seed) + 0.5));
            color.rgb += (first + second - 1.0) * lighting.shading.z * (1.0 / 255.0);
        }
        color.rgb = clamp(color.rgb, 0.0, 1.0);
    }

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
