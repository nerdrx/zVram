#version 450
layout(set = 0, binding = 0, std430) readonly buffer Vertices { vec2 positions[]; };
void main() { gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0); }
