#version 320 es
in vec2 vUV;
in vec4 vColor;
flat in int vShape;
in float vSeed;
out vec4 oColor;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}

void main() {
    float d = length(vUV);
    float m;
    if (vShape == 0) {                       // brilho suave
        m = pow(max(0.0, 1.0 - d), 2.0);
    } else if (vShape == 1) {                // fumaça (borda irregular)
        float n = noise(vUV * 2.5 + vSeed * 13.0) * 0.55 + noise(vUV * 5.0 - vSeed * 7.0) * 0.45;
        m = smoothstep(1.0, 0.15, d) * (0.35 + 0.8 * n);
    } else if (vShape == 2) {                // faísca / bala (o vertex shader alonga)
        m = pow(max(0.0, 1.0 - d), 3.0);
    } else if (vShape == 3) {                // anel de onda de choque
        m = exp(-pow((d - 0.72) / 0.12, 2.0));
    } else if (vShape == 4) {                // estrela / clarão
        float rays = max(exp(-abs(vUV.x) * 16.0), exp(-abs(vUV.y) * 16.0)) * max(0.0, 1.0 - d);
        m = max(pow(max(0.0, 1.0 - d), 4.0) * 1.5, rays);
    } else if (vShape == 5) {                // cristal de gelo
        float k = abs(vUV.x) * 1.9 + abs(vUV.y);
        m = smoothstep(1.0, 0.75, k) * (0.7 + 0.3 * step(0.0, vUV.x * vUV.y));
    } else {                                 // chama (gota com ruído subindo)
        vec2 q = vec2(vUV.x * (1.0 + (vUV.y + 1.0) * 0.65), vUV.y * 0.9 + 0.1);
        float n = noise(vec2(vUV.x * 3.0, vUV.y * 2.0 - vSeed * 20.0));
        m = smoothstep(1.0, 0.25, length(q) + n * 0.4);
    }
    if (m < 0.004) discard;
    oColor = vColor * m;  // pré-multiplicado: partícula aditiva tem alpha 0
}
