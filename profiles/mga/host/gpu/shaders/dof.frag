#version 450

// Depth of field, drawn at the seam between the world and the interface.
//
// It has to be here and not in the post pass for the same reason the ambient
// occlusion does: by the time the post pass runs, the heads-up display has been
// drawn into the same target, and blurring there would blur the interface along
// with the room behind it.
//
// What this is, stated plainly
// ----------------------------
//
// A far-field blur with an autofocus plane, not a simulation of a lens.
// Everything nearer than the focal plane stays sharp; everything beyond it
// softens with distance and stops softening at a set spread. A real lens blurs
// the near field too, and doing that here would mean blurring whatever the
// camera is closest to, which in a third-person game is usually the player's
// own shoulder. The asymmetry is the point, not a shortcut.
//
// The focal plane is the depth under the middle of the screen, read here rather
// than fed in, so there is no round trip to the host and no frame of latency.
// Five taps rather than one, so a thin object crossing the centre -- a railing,
// a doorframe -- cannot yank the focus for a frame. What it does NOT have is
// smoothing across frames: a camera cut refocuses instantly where a real lens
// would take a moment. Keeping history would mean reading one pixel back to the
// host, which is cheap but is a fence and a frame of lag, and an instant
// refocus on a cut is what the eye does anyway.
//
// The colour comes from a half-size copy of the finished world. Half size is
// not a compromise made for speed: a blur of radius r on a half-size image is a
// blur of radius 2r for a quarter of the taps, which is the same trick the
// bloom chain uses and the reason it can afford to be wide.
//
// The sharp picture is never sampled at all. It is already in the attachment,
// and the pipeline blends with SRC_ALPHA: this shader hands over the blurred
// colour and how much of it to use, and the hardware mixes it with what is
// there. That is what makes a full-size copy unnecessary.

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

// The half-size copy of the world, linear-sampled.
layout(set = 0, binding = 0) uniform sampler2D blurred;
// The depth the world left, at full size, nearest-sampled.
layout(set = 0, binding = 1) uniform sampler2D scene_depth;

layout(push_constant) uniform Push {
    // xy: one texel of the half-size copy. zw: one texel of the depth buffer.
    vec4 texel;
    // x: +1 when a larger depth value means nearer, -1 otherwise.
    // y: the strength, 0 for off -- it scales the blur radius, not the mix, so
    //    turning it down makes the background less soft rather than making a
    //    soft background more transparent.
    // z: the spread: how far past the focal plane, as a fraction of what is
    //    left between it and the far plane, the blur takes to saturate.
    // w: the widest blur radius, in texels of the half-size copy.
    vec4 params;
} push;

// Depth as a number that is 0 at the near plane and 1 at the far plane,
// whichever way round the GE left the buffer. Every comparison below is in
// these units, so none of them has to know which way it was.
float distance_of(vec2 uv) {
    const float raw = texture(scene_depth, clamp(uv, 0.0, 1.0)).r;
    return push.params.x > 0.0 ? 1.0 - raw : raw;
}

// How out of focus a point at this distance is, from 0 to 1.
float confusion(float distance_here, float focus) {
    // Nearer than the focal plane is sharp, always. See the note above.
    if (distance_here <= focus) return 0.0;
    // The spread is a fraction of what is left between the focal plane and the
    // far plane, not a fixed number of depth units. A fixed one would behave
    // completely differently depending on where the focus happened to land,
    // because a depth buffer is not linear and its far half holds almost no
    // range at all.
    const float behind = max(1.0 - focus, 1e-4) * max(push.params.z, 1e-3);
    return clamp((distance_here - focus) / behind, 0.0, 1.0);
}

void main() {
    const float here = distance_of(frag_uv);

    // The focal plane: an average over a small region at the middle of the
    // screen, not the nearest of a few taps at the exact centre.
    //
    // Taking the nearest was meant to stop a railing across the centre from
    // pulling focus, and it does the opposite. The nearest of five samples IS
    // the railing, so a thin object crossing the middle of the frame captures
    // the focal plane, the whole background blurs, and when it passes the
    // focus snaps back and the blur vanishes. Panning the camera across a
    // scene with anything vertical in it -- a post, a doorframe, a fence --
    // makes the defocus switch on and off, which is what a depth of field must
    // never do, because the eye reads a change in blur as a change in what it
    // is looking at.
    //
    // An average over a region is stable under exactly that motion: one thin
    // object entering it moves the mean a little rather than replacing it.
    // Samples at the far plane are left out, so an open sky over a courtyard
    // does not drag the focus to infinity and blur the ground the camera is
    // actually pointed at.
    //
    // This is not temporal smoothing and does not pretend to be. A cut still
    // refocuses in one frame, which is what the eye does anyway; what it fixes
    // is the focus changing when the scene did not.
    const vec2 middle = vec2(0.5);
    const float step_x = push.texel.z * 10.0;
    const float step_y = push.texel.w * 10.0;
    float focus_sum = 0.0;
    float focus_weight = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            const float sampled = distance_of(middle + vec2(float(x) * step_x, float(y) * step_y));
            // 0.999 rather than 1.0: the far plane is not always exactly one
            // once a projection has been through a reversed-z remap.
            if (sampled >= 0.999) continue;
            focus_sum += sampled;
            focus_weight += 1.0;
        }
    }
    // Nothing but sky in the middle of the frame: there is no subject to focus
    // on, so nothing is blurred rather than everything being.
    if (focus_weight <= 0.0) {
        out_color = vec4(0.0);
        return;
    }
    const float focus = focus_sum / focus_weight;

    const float coc = confusion(here, focus) * clamp(push.params.y, 0.0, 1.0);
    if (coc <= 0.004) {
        // Below a quarter of a display step the mix would round to nothing.
        // Writing zero alpha leaves the attachment exactly as it was.
        out_color = vec4(0.0);
        return;
    }

    const float radius = coc * max(push.params.w, 1.0);
    // A golden-angle spiral: the taps spread evenly instead of landing in
    // rings, so what softens looks like defocus rather than like a pattern.
    const float kGolden = 2.39996323;
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = 0; i < 16; ++i) {
        const float t = (float(i) + 0.5) / 16.0;
        const float r = sqrt(t) * radius;
        const float angle = float(i) * kGolden;
        const vec2 offset = vec2(cos(angle), sin(angle)) * r * push.texel.xy;
        const vec2 at = clamp(frag_uv + offset, 0.0, 1.0);
        // The tap's own blur, not this pixel's.
        //
        // Without this the blur reaches backwards: a sharp figure standing in
        // front of a distant wall has its edge pixels dragged out into the
        // wall's blur, and what you see is a halo of the character smeared
        // across the background. Weighting each tap by how out of focus IT is
        // means a sharp foreground contributes almost nothing to a blurred
        // background, while the background blurs into itself freely.
        const float tap = confusion(distance_of(at), focus);
        // The +0.02 is so a tap that is barely blurred still counts for
        // something: at exactly zero, a background that is only just past the
        // focal plane would have no taps at all and fall back to the centre.
        const float weight = tap + 0.02;
        sum += texture(blurred, at).rgb * weight;
        total += weight;
    }
    // Alpha is the mix, and the blend does the mixing against the sharp
    // picture that is already in the attachment.
    out_color = vec4(total > 0.0 ? sum / total : texture(blurred, frag_uv).rgb, coc);
}
