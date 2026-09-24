#version 450

// The post-processing pass. It replaces the blit that used to copy the game's
// render target onto the swapchain, so it also does the scaling and the
// letterboxing the blit did: everything outside the game rectangle is black.
//
// Doing it as a draw rather than a blit is what makes room for effects that
// need to read more than one texel -- anti-aliasing here, and screen-space
// ambient occlusion once the depth target is bound alongside the colour.

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D scene_depth;

layout(push_constant) uniform Push {
    // The game rectangle inside the swapchain, in normalised coordinates:
    // xy = top-left, zw = size. Outside it the screen is black.
    vec4 rect;
    // 1 / the scene target's size, for neighbour taps.
    vec2 texel;
    // x: effects (1 = FXAA), y: colour grading strength, 0 when off.
    vec2 effects;
    // x: +1 when a larger depth value means nearer, -1 otherwise -- the GE's
    // viewport decides it, and the game does reverse the range in places.
    // y: the tap radius in target pixels. z: the depth an untouched pixel
    // holds. w: contact shadow strength, 0 for off.
    vec4 depth;
} push;

// Rec.709 luma, which is what FXAA's edge test wants.
float luma(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// FXAA 3.11's console-quality variant: cheap, and enough for the long near-
// horizontal and near-vertical edges PSP-era geometry is full of. It finds the
// local contrast, and where an edge runs through the pixel it takes a second
// pair of taps along the edge's normal and blends towards them.
vec3 fxaa(vec2 uv) {
    const float kEdgeThreshold = 0.125;     // below this it is not an edge
    const float kEdgeThresholdMin = 0.0312; // dark pixels are noisy; ignore them
    const float kSpanMax = 8.0;
    const float kReduceMul = 1.0 / 8.0;
    const float kReduceMin = 1.0 / 128.0;

    const vec3 middle = texture(scene_color, uv).rgb;
    const float luma_m = luma(middle);
    const float luma_nw = luma(texture(scene_color, uv + vec2(-push.texel.x, -push.texel.y)).rgb);
    const float luma_ne = luma(texture(scene_color, uv + vec2(push.texel.x, -push.texel.y)).rgb);
    const float luma_sw = luma(texture(scene_color, uv + vec2(-push.texel.x, push.texel.y)).rgb);
    const float luma_se = luma(texture(scene_color, uv + vec2(push.texel.x, push.texel.y)).rgb);

    const float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
    const float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));
    const float range = luma_max - luma_min;
    // Flat enough to leave alone. This is what keeps the HUD and the card art
    // from being softened: large areas of flat colour never reach the
    // threshold, and only their edges do.
    if (range < max(kEdgeThresholdMin, luma_max * kEdgeThreshold)) return middle;

    vec2 direction = vec2(-((luma_nw + luma_ne) - (luma_sw + luma_se)),
                          ((luma_nw + luma_sw) - (luma_ne + luma_se)));
    const float reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * 0.25 * kReduceMul, kReduceMin);
    const float scale = 1.0 / (min(abs(direction.x), abs(direction.y)) + reduce);
    direction = clamp(direction * scale, vec2(-kSpanMax), vec2(kSpanMax)) * push.texel;

    const vec3 near = 0.5 * (texture(scene_color, uv + direction * (1.0 / 3.0 - 0.5)).rgb +
                             texture(scene_color, uv + direction * (2.0 / 3.0 - 0.5)).rgb);
    const vec3 far = near * 0.5 + 0.25 * (texture(scene_color, uv + direction * -0.5).rgb +
                                          texture(scene_color, uv + direction * 0.5).rgb);
    const float luma_far = luma(far);
    // The wider pair is better on long edges but overshoots on short ones, so
    // it is used only while it stays inside the local range.
    return (luma_far < luma_min || luma_far > luma_max) ? near : far;
}

// Contact shadows out of the depth buffer.
//
// This is not textbook screen-space ambient occlusion, and it cannot be:
// there is no normal buffer, and no single projection matrix to rebuild
// view-space positions from, because the GE's projection changes from draw to
// draw. What there is is the depth of every pixel, and that is enough to find
// the creases -- where a character meets the floor, where a wall meets it --
// which is most of what ambient occlusion is actually seen to do.
//
// `here` and `slope` are sampled by the caller before any branching: taking a
// derivative where the quad is partly outside the picture is undefined, and
// the letterbox test in main() is exactly such a branch.
float contact_shadow(vec2 uv, float here, float slope) {
    const float toward = push.depth.x;
    const float radius = push.depth.y;
    // Nothing was drawn on this pixel, so there is no crease to find.
    if (abs(here - push.depth.z) < 1e-6) return 0.0;

    const float front = here * toward;
    // The depth buffer is not linear, so one fixed threshold would catch
    // everything close to the camera and nothing far from it. The depth's
    // screen-space gradient is the local surface slope, and it scales the same
    // way the depth does, so it gives a threshold that holds at any distance.
    //
    // But it has to have a floor, and the floor has to be the depth's own
    // precision. The PSP's depth buffer is 16 bits, so whatever the host
    // target holds, the values in it come in steps of about 1/65535. On a flat
    // surface facing the camera the slope is nearly zero, and a threshold
    // below one step turns ordinary quantisation into creases: neighbouring
    // pixels differ by one step, the test passes, and the whole surface
    // crawls. That is what made everything glimmer.
    const float quantum = 1.0 / 65535.0;
    const float bias = max(slope * 2.0, quantum * 6.0);

    const vec2 ring[8] = vec2[8](vec2(1.0, 0.0), vec2(0.7071, 0.7071), vec2(0.0, 1.0), vec2(-0.7071, 0.7071),
                                 vec2(-1.0, 0.0), vec2(-0.7071, -0.7071), vec2(0.0, -1.0), vec2(0.7071, -0.7071));
    float occlusion = 0.0;
    for (int i = 0; i < 8; ++i) {
        const float neighbour = texture(scene_depth, uv + ring[i] * radius * push.texel).r * toward;
        // Positive when the neighbour stands in front of this pixel.
        const float delta = neighbour - front;
        // In front by more than the surface's own slope explains: a crease.
        // In front by far more than that: a silhouette against something in
        // the distance, which must not be given a dark halo.
        occlusion += clamp((delta - bias) / (bias * 8.0), 0.0, 1.0) *
                     (1.0 - smoothstep(bias * 32.0, bias * 96.0, delta));
    }
    return occlusion * 0.125;
}

// A gentle grade. PSP art was authored for a small, dim, low-contrast screen,
// and on a modern display it reads as washed out -- not because the colours are
// wrong but because the range they were made for was narrower than the one they
// are shown in now.
//
// So: a little more contrast about mid grey, and a little more saturation. Both
// deliberately mild, and both applied to the whole picture including the
// interface, because grading part of a frame is what makes a grade obvious.
// Overdone, this is the effect that makes an old game look like a bad filter
// rather than a better screen, which is why the strength is a setting and the
// default is subtle.
vec3 grade(vec3 color, float strength) {
    const float kContrast = 0.12;
    const float kSaturation = 0.18;
    // Around 0.5 rather than 0, so raising contrast does not also brighten.
    vec3 contrasted = clamp((color - 0.5) * (1.0 + kContrast * strength) + 0.5, 0.0, 1.0);
    float grey = luma(contrasted);
    return clamp(mix(vec3(grey), contrasted, 1.0 + kSaturation * strength), 0.0, 1.0);
}

void main() {
    const vec2 uv = (frag_uv - push.rect.xy) / push.rect.zw;
    // Both of these have to be read before the letterbox test below, because
    // fwidth() is only defined where the whole quad takes the same branch.
    const float here = texture(scene_depth, clamp(uv, 0.0, 1.0)).r;
    const float slope = fwidth(here);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 color = push.effects.x > 0.5 ? fxaa(uv) : texture(scene_color, uv).rgb;
    if (push.depth.w > 0.0) {
        const float shade = clamp(contact_shadow(uv, here, slope) * push.depth.w, 0.0, 0.8);
        color *= 1.0 - shade;
    }
    if (push.effects.y > 0.0) color = grade(color, push.effects.y);
    out_color = vec4(color, 1.0);
}
