#version 450
layout(location=0) out vec2 uv;
void main() {
    vec2 p = gl_VertexIndex == 1 ? vec2(3,-1) :
             gl_VertexIndex == 2 ? vec2(-1,3) : vec2(-1,-1);
    gl_Position = vec4(p, 0, 1);
    uv = p * 0.5 + 0.5;
}
