#version 320 es
// Linhas do editor (gizmo, grade, seleção). Posições já em clip space, calculadas na CPU.
layout(location = 0) in vec4 aPos;
layout(location = 1) in vec4 aColor;
out vec4 vColor;
void main() { vColor = aColor; gl_Position = aPos; }
