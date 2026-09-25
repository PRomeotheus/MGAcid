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
// The colour grade, as a table: the colour goes in as a coordinate and the
// graded colour comes out. Always bound -- the identity table when no grade is
// loaded -- so this shader never has to branch on whether it exists.
layout(set = 0, binding = 2) uniform sampler3D colour_lut;

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
    // x: bloom strength, 0 for off; y: the brightness it starts from;
    // z: its radius in target pixels; w: the grading table's size when one is
    // loaded from a file, 0 when the built-in grade should run instead.
    vec4 bloom_params;
    // x: sharpening strength, 0 for off; y, z, w spare.
    vec4 sharpen;
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

// The glow around bright things.
//
// A lens scatters a little of the light from a bright source across everything
// near it, and a camera sensor blooms outright. Neither happens on a PSP, and
// Metal Gear Ac!d is a game of dark rooms with hard lights in them, which is
// the case where the absence reads as flatness.
//
// The honest limitation: this is one pass over the finished frame, sampling a
// spiral of taps. A proper implementation builds a chain of half-size images and
// blurs each, which is both wider and cheaper -- the blur gets large for free
// because the image is small. Doing that here would mean new render passes and
// new attachments; this stays inside the pass that already exists, and pays for
// its width in taps instead. It is good for a modest glow and would band
// visibly if pushed to a wide one, which is why the strength stays low.
//
// Only light above the threshold contributes, so a normally lit surface glows
// not at all and a lamp or a muzzle flash glows a lot. Without that, bloom is
// just a blur mixed into the whole picture -- which is the look people mean when
// they say a game has too much bloom.
vec3 bloom(vec2 uv, float threshold, float radius) {
    // A golden-angle spiral: twenty-four taps that spread evenly rather than
    // landing in rings, which is what keeps the falloff smooth instead of
    // showing the sampling pattern as concentric bands.
    const float kGolden = 2.39996323;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = 0; i < 24; ++i) {
        float t = (float(i) + 0.5) / 24.0;
        float r = sqrt(t) * radius;
        float angle = float(i) * kGolden;
        vec2 offset = vec2(cos(angle), sin(angle)) * r * push.texel;
        // Gaussian in the radius, so the near taps carry the glow and the far
        // ones only the faintest halo.
        float weight = exp(-2.0 * t);
        vec3 sample_color = texture(scene_color, clamp(uv + offset, 0.0, 1.0)).rgb;
        sum += max(sample_color - threshold, vec3(0.0)) * weight;
        total += weight;
    }
    return total > 0.0 ? sum / total : vec3(0.0);
}

// Contrast-adaptive sharpening.
//
// The game draws 480x272 and it is being shown several times that size. Every
// magnification filter that does not produce stair-steps produces softness
// instead; that is the same trade seen from the two ends, and no filter
// escapes it. Sharpening is how the softness is taken back out.
//
// What makes this the adaptive kind rather than an unsharp mask: the amount is
// decided per pixel from how much room the neighbourhood has left. A plain
// unsharp mask applies the same amount everywhere, so wherever a light edge
// meets a dark one it pushes the light side past white and the dark side below
// black, and what is left is the pale outline that makes a picture look
// sharpened rather than sharp. Here the strength falls away as the local
// values approach either end, so an edge that is already at white gets almost
// nothing and a mid-grey edge gets the most.
//
// The taps are the four neighbours rather than all eight. The diagonals cost
// four more samples and mostly restate what the cross already says at this
// scale, where a single game pixel covers several screen pixels.
//
// This reads the scene as it was drawn, not the frame as this shader has been
// building it, because the neighbours of a pixel are only available from the
// texture. So what it produces is a correction -- the difference sharpening
// would have made to the original -- and that difference is added to the
// finished colour. Anti-aliasing, contact shadows and bloom are all modest
// changes to each pixel, so the correction is still close to right, and the
// alternative is a second pass over a second target for a sharpen.
vec3 sharpen_delta(vec2 uv, float strength) {
    const vec3 here = texture(scene_color, uv).rgb;
    const vec3 up = texture(scene_color, uv + vec2(0.0, -push.texel.y)).rgb;
    const vec3 down = texture(scene_color, uv + vec2(0.0, push.texel.y)).rgb;
    const vec3 left = texture(scene_color, uv + vec2(-push.texel.x, 0.0)).rgb;
    const vec3 right = texture(scene_color, uv + vec2(push.texel.x, 0.0)).rgb;

    const vec3 lowest = min(here, min(min(up, down), min(left, right)));
    const vec3 highest = max(here, max(max(up, down), max(left, right)));
    // How much headroom the neighbourhood has: how far the darkest tap is from
    // black against how far the brightest is from white, whichever is worse.
    // Near either end this goes to zero and the sharpening goes with it.
    const vec3 headroom = min(lowest, vec3(1.0) - highest) / max(highest, vec3(1.0 / 255.0));
    // The square root makes the falloff gradual rather than sudden, so the
    // amount does not visibly change along an edge that runs from mid grey
    // into a highlight.
    const vec3 amount = sqrt(clamp(headroom, 0.0, 1.0));
    // The weight each neighbour is subtracted with, between -1/8 and -1/5.
    //
    // The upper end is not a matter of taste. The convolution below divides by
    // 4 * weight + 1, so a weight of -1/4 makes that exactly zero, and at full
    // amplitude the result is a division by zero -- which on a GPU is an
    // infinity or a NaN and reaches the screen as a white or black speck on
    // the sharpest edges in the picture. -1/5 keeps the divisor at 1/5 or
    // above at every amplitude. (I had -1/4 here first and found it by
    // measuring rather than by reading, which is the only reason it is not
    // still here.)
    const vec3 weight = -amount / mix(8.0, 5.0, clamp(strength, 0.0, 1.0));
    const vec3 sum = up + down + left + right;
    // A normalised convolution, so a flat neighbourhood comes back unchanged
    // whatever the weight is.
    const vec3 sharpened = (sum * weight + here) / (4.0 * weight + vec3(1.0));
    return sharpened - here;
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
// The grade as a table, which is what a .cube holds.
//
// The half-texel business is not a detail to gloss: a 3D texture's texel
// centres sit half a texel in from each face, so sampling at the colour itself
// would put 0 and 1 half a texel outside the cube. Clamping then returns the
// end entries for a whole half-texel band, and black and white -- the two
// colours a grade is most often judged by -- would be the two it got wrong.
// Scaling into (size-1)/size and offsetting by half puts the first entry
// exactly at 0 and the last exactly at 1.
vec3 lut_grade(vec3 color, float size) {
    vec3 scaled = clamp(color, 0.0, 1.0) * ((size - 1.0) / size) + (0.5 / size);
    return texture(colour_lut, scaled).rgb;
}

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
    // Before the grade: bloom belongs to the picture, grading is how the
    // picture is shown.
    if (push.bloom_params.x > 0.0) {
        vec3 glow = bloom(uv, clamp(push.bloom_params.y, 0.0, 1.0), max(push.bloom_params.z, 1.0));
        color = min(color + glow * push.bloom_params.x, vec3(1.0));
    }
    // Last of the things that change the picture, and after the two that
    // soften it: anti-aliasing blends across edges and bloom spreads light,
    // and taking the softness back out before they put it in would be the
    // wrong order.
    if (push.sharpen.x > 0.0) color = clamp(color + sharpen_delta(uv, push.sharpen.x), 0.0, 1.0);
    if (push.effects.y > 0.0) {
        // A table is a whole grade, so the strength dial blends the picture
        // towards it. The built-in grade takes the strength inside itself
        // instead, where it scales the two constants -- which is the same
        // thing for the contrast and not for the saturation, and is left
        // exactly as it was so that turning a table on and off is the only
        // difference this change makes to a picture that had no table.
        color = push.bloom_params.w > 0.0
                    ? mix(color, lut_grade(color, push.bloom_params.w), push.effects.y)
                    : grade(color, push.effects.y);
    }
    out_color = vec4(color, 1.0);
}
