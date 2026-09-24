#version 450

// Ambient occlusion, drawn into the scene while the scene is still only the
// scene.
//
// The same darkening used to live in the post pass, which runs on the finished
// frame -- and the finished frame has the interface painted on it. The depth
// buffer under a HUD panel still holds whatever 3D stands behind it, so the
// crease test found creases there and drew them across the panel. No amount of
// tuning fixes that: the information needed to tell interface from world is
// gone by the time the post pass runs.
//
// So this pass runs at the seam instead -- after the last piece of world
// geometry and before the first flat draw -- where the depth buffer describes
// the world and nothing else, and where anything drawn afterwards covers the
// result rather than being shaded by it. That is the same seam the blob shadows
// use, for the same reason.
//
// It writes the light that survives, not the darkness added, and the pipeline
// blends it as a multiply. So the pass owns no copy of the colour target: it
// reads depth, and the blend does the rest.

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_depth;

layout(push_constant) uniform Push {
    // 1 / the scene target's size, for neighbour taps.
    vec2 texel;
    // x: +1 when a larger depth value means nearer, -1 otherwise -- the GE's
    // viewport decides it, and the game does reverse the range in places.
    // y: the tap radius in target pixels. z: the depth an untouched pixel
    // holds. w: strength.
    vec4 depth;
} push;

// Creases out of the depth buffer.
//
// Not textbook screen-space ambient occlusion, and it cannot be: there is no
// normal buffer, and no single projection matrix to rebuild view-space
// positions from, because the GE's projection changes from draw to draw. What
// there is is the depth of every pixel, and that is enough to find the creases
// -- where a character meets the floor, where a wall meets it -- which is most
// of what ambient occlusion is actually seen to do.
float occlusion_at(vec2 uv, float here, float slope) {
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
    // The floor under it is the depth's own precision. The PSP's depth buffer
    // is 16 bits, so whatever the host target holds, the values in it come in
    // steps of about 1/65535. On a flat surface facing the camera the slope is
    // nearly zero, and a threshold below one step turns ordinary quantisation
    // into creases: neighbouring pixels differ by one step, the test passes,
    // and the whole surface crawls.
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

void main() {
    // This pass covers the scene target exactly, so there is no letterbox and
    // no branch before the derivative below -- which is what made the post
    // pass read its depth up front.
    const vec2 uv = clamp(frag_uv, 0.0, 1.0);
    const float here = texture(scene_depth, uv).r;
    const float slope = fwidth(here);
    const float shade = clamp(occlusion_at(uv, here, slope) * push.depth.w, 0.0, 0.8);
    // The surviving light, multiplied onto what is already there. Alpha is 1
    // so a target whose alpha the game reads back is left as it was.
    out_color = vec4(vec3(1.0 - shade), 1.0);
}
