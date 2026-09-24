#version 450

// The shadow map's only shader. The positions arrive already in draw space --
// the space the GE's lighting and every world matrix in the frame share -- so
// one matrix takes them the whole way to the light's clip space. There is no
// fragment shader: the pass writes depth and nothing else.

layout(location = 0) in vec4 in_position;

layout(push_constant) uniform Push {
    mat4 light_transform;
} push;

void main() {
    vec4 clip = push.light_transform * vec4(in_position.xyz, 1.0);
    // The projection below is built for OpenGL's z in [-w, w], as the PSP's
    // own matrices are, so the same remap the scene's vertices get applies
    // here. Without it the near half of the light's box is clipped away.
    clip.z = (clip.z + clip.w) * 0.5;
    gl_Position = clip;
}
