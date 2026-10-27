#version 320 es
// Partícula = quad instanciado. Por instância: posição+tamanho, cor (pré-multiplicada), velocidade+esticamento, extras.
layout(location = 0) in vec4 iPosSize;
layout(location = 1) in vec4 iColor;
layout(location = 2) in vec4 iVel;    // xyz velocidade, w esticamento (s): > 0 alonga na direção do movimento
layout(location = 3) in vec4 iMisc;   // x rotação, y formato, z semente

uniform vec3 uRight;
uniform vec3 uUp;

out vec2 vUV;
out vec4 vColor;
flat out int vShape;
out float vSeed;

void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    vUV = c;
    vColor = iColor;
    vShape = int(iMisc.y + 0.5);
    vSeed = iMisc.z;
    vec3 p = iPosSize.xyz;
    float s = iPosSize.w;
    float speed = length(iVel.xyz);
    vec3 world;
    if (iVel.w > 0.0 && speed > 1e-3) {
        vec3 along = iVel.xyz / speed;
        vec3 toCam = normalize(uCamPos.xyz - p);
        vec3 side = cross(along, toCam);
        float sl = length(side);
        side = sl > 1e-4 ? side / sl : uRight;
        world = p + side * c.x * s + along * c.y * (s + speed * iVel.w);
    } else {
        float cr = cos(iMisc.x), sr = sin(iMisc.x);
        vec2 rc = vec2(c.x * cr - c.y * sr, c.x * sr + c.y * cr);
        world = p + (uRight * rc.x + uUp * rc.y) * s;
    }
    gl_Position = uViewProj * vec4(world, 1.0);
}
