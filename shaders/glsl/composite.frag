#version 450
// LoucuraXXXX GPU - final composite / present fragment stage.
// Samples the rendered image and writes it to the presentation target. Kept
// separate from the filter passes so presentation is a distinct, testable step.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler2D in_image;
void main() {
    out_color = texture(in_image, v_uv);
}
