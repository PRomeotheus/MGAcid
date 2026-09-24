#version 450

// A fullscreen triangle, larger than the screen so no seam runs down the
// middle the way a two-triangle quad's diagonal can. Three vertices, no vertex
// buffer: the positions come from the vertex index.
layout(location = 0) out vec2 frag_uv;

void main() {
    const vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    frag_uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
