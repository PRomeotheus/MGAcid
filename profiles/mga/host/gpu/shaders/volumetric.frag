#version 450

// Light in the air, rather than light on a surface.
//
// The shadow map already answers the only question this needs: for a point in
// the world, was anything standing between it and the light. A surface asks it
// once, where the surface is. This asks it repeatedly along the line from the
// eye to that surface, and adds up how much of that line the light reaches.
// Where the line runs through open light the sum is high and the air glows;
// where it runs behind a wall or a railing the sum drops and a shaft appears
// with the caster's shape cut out of it. Nothing else is needed -- no new
// geometry, no second map, and no knowledge of where the light is.
//
// It runs at the same seam the occlusion pass runs at, in the same render pass,
// for the same reason: the depth buffer there describes the world and nothing
// else, and anything drawn afterwards covers the result instead of being lit by
// it. Otherwise shafts would appear over the interface, which has depth values
// behind it that mean nothing.
//
// The pass blends additively, because in-scattered light is light arriving on
// top of what is already there rather than a tint over it.

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D scene_depth;
layout(set = 0, binding = 1) uniform sampler2D shadow_map;

layout(push_constant) uniform Push {
    // The scene's clip space straight to the light's, so the march never needs
    // world space and never needs two matrices. See below for why that is not
    // as lossy as it sounds.
    mat4 clip_to_light;
    // The row of the inverse view-projection that gives w. The division by it
    // is what makes the march uniform in the world rather than in clip space.
    vec4 inverse_w;
    // x: strength, y: steps, z: the depth value of the near plane, w: a phase
    // that moves the dither each frame.
    vec4 params;
    vec4 light_color;
} push;

void main() {
    float depth = texture(scene_depth, frag_uv).r;
    vec2 ndc = frag_uv * 2.0 - 1.0;
    vec4 clip_near = vec4(ndc, push.params.z, 1.0);
    vec4 clip_far = vec4(ndc, depth, 1.0);

    // A point on the near plane and the point the eye actually landed on, both
    // in the light's clip space.
    //
    // The division is the whole trick. clip_to_light * clip gives the light
    // space position of the world point scaled by that world point's own w, and
    // dividing it out recovers the unscaled position. Having done that at both
    // ends, a straight interpolation between them is a straight line in the
    // WORLD, which is what the march has to be for equal steps to mean equal
    // distances. Interpolating the two clip positions instead would bunch every
    // sample up against the near plane.
    float w_near = dot(push.inverse_w, clip_near);
    float w_far = dot(push.inverse_w, clip_far);
    if (abs(w_near) < 1e-9 || abs(w_far) < 1e-9) {
        out_color = vec4(0.0);
        return;
    }
    vec4 light_near = (push.clip_to_light * clip_near) / w_near;
    vec4 light_far = (push.clip_to_light * clip_far) / w_far;
    // The halved-z convention the shadow pass rasterised with. It is affine in
    // z and w, so applying it at the two ends and interpolating gives the same
    // answer as applying it at every step.
    light_near.z = (light_near.z + light_near.w) * 0.5;
    light_far.z = (light_far.z + light_far.w) * 0.5;

    int steps = clamp(int(push.params.y + 0.5), 4, 64);
    // Without this every pixel samples at the same depths and the march shows
    // as fixed bands hanging in the air. Offsetting each pixel's start by a
    // fraction of a step trades those bands for noise, which the eye forgives
    // far more readily.
    //
    // Which noise matters more than it looks. The usual sin-of-a-dot hash
    // gives coarse, blotchy grain here -- neighbouring pixels land on unrelated
    // values, so the error clumps at low frequencies where the eye is most
    // sensitive. Jimenez's interleaved gradient noise puts the error at a high
    // frequency instead, and reads as a fine weave rather than as grain. Judged
    // by eye: a local-difference measure prefers the hash, because it rewards
    // smooth blotches over fine structure, which is the wrong way round.
    float dither = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))
                         + push.params.w);

    float reached = 0.0;
    // How many samples landed inside the light's box at all, and how many of
    // those were in shadow. A shaft is lit air with unlit air beside it, and
    // without the second number there is no way to tell one from a sky.
    float inside = 0.0;
    for (int i = 0; i < steps; ++i) {
        float t = (float(i) + dither) / float(steps);
        vec4 point = mix(light_near, light_far, t);
        if (point.w <= 0.0) continue;
        vec3 projected = point.xyz / point.w;
        vec2 uv = projected.xy * 0.5 + 0.5;
        // Outside the light's box nothing was recorded. Treating that as unlit
        // rather than lit matters: the box is only as big as the casters, so
        // calling the space outside it lit would put a glow over the whole
        // scene and hide the shafts inside it.
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) continue;
        if (projected.z < 0.0 || projected.z > 1.0) continue;
        inside += 1.0;
        reached += projected.z <= texture(shadow_map, uv).r ? 1.0 : 0.0;
    }

    // A ray with nothing in its way contributes nothing.
    //
    // The note above about the box being only as big as the casters stopped
    // being true: it is deliberately sized to hold every caster in the level,
    // so that characters cast wherever they are. That leaves nearly every
    // sample of air inside it and lit, and the lit fraction alone is then
    // close to one for the whole screen -- a flat glow over everything rather
    // than shafts, which is what the box guard used to prevent by accident.
    //
    // So the lit fraction is scaled by how much of the ray was blocked.
    // Nothing blocked means open air, which scatters no more here than
    // anywhere else and so reads as nothing; a ray that passes a wall or a
    // gantry keeps its lit part and shows it against the dark beside it. The
    // smoothstep rather than a test, so a shaft fades in at its edge instead
    // of switching on a pixel at a time.
    float blocked = inside > 0.0 ? 1.0 - reached / inside : 0.0;
    float shaft = smoothstep(0.0, 0.15, blocked);
    float scatter = reached / float(steps) * shaft * push.params.x;
    out_color = vec4(push.light_color.rgb * scatter, 1.0);
}
