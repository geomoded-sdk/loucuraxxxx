#version 450
// LoucuraXXXX GPU - fullscreen triangle vertex stage.
//
// Emits a single oversized triangle covering the viewport without a vertex
// buffer. Used by every full-screen pass (filters, composite, present).

layout(location = 0) out vec2 v_uv;

void main() {
    // Three vertices: (-1,-1), (3,-1), (-1,3) in clip space.
    vec2 positions[3] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    vec2 position = positions[gl_VertexIndex];
    // Map clip space to [0,1] UV, flipping Y so texture origin is top-left.
    v_uv = vec2((position.x + 1.0) * 0.5, 1.0 - (position.y + 1.0) * 0.5);
    gl_Position = vec4(position, 0.0, 1.0);
}
