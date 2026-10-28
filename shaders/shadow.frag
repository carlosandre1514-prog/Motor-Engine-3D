#version 320 es
in vec3 vWorldPos;
flat in uint vCut;
void main() {  // apenas profundidade (a luz passa pelas aberturas)
    if (vCut != 0u && insideCut(vWorldPos, vCut)) discard;
}
