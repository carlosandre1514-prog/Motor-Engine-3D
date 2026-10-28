// Incluído automaticamente pelo Renderer logo após "#version" e os #defines de variante.
precision highp float;
precision highp int;

struct PointLightData { vec4 posRadius; vec4 colorIntensity; };

layout(std140, binding = 0) uniform SceneBlock {
    mat4 uViewProj;
    vec4 uCamPos;
    vec4 uDirDir;            // xyz direção do sol, w = sombras on/off
    vec4 uDirColor;          // rgb * intensidade
    mat4 uCascadeVP[3];
    vec4 uCascadeSplits;     // xyz fim de cada cascata, w distância de sombra
    vec4 uCascadeTexel;      // tamanho do texel em mundo por cascata
    vec4 uEnvParams;         // x exposição, y mip máximo do cubemap, z intensidade do IBL
    vec4 uSH[9];             // irradiância do céu (SH9, já / PI)
    vec4 uFog;               // rgb cor da neblina, w densidade
    ivec4 uCounts;           // x = nº de point lights
    PointLightData uPoints[8];
};

// Aberturas cortadas em paredes (porta/janela/vão): caixa do mundo -> cubo unitário. Bits no objeto dizem quais valem.
layout(std140, binding = 2) uniform CutBlock { mat4 uCuts[32]; };
bool insideCut(vec3 p, uint mask) {
    for (int i = 0; i < 32; ++i) {
        if ((mask & (1u << uint(i))) == 0u) continue;
        vec3 q = (uCuts[i] * vec4(p, 1.0)).xyz;
        if (all(lessThan(abs(q), vec3(0.5)))) return true;
    }
    return false;
}

vec3 acesTonemap(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 toDisplay(vec3 hdr) { return pow(acesTonemap(hdr * uEnvParams.x), vec3(1.0 / 2.2)); }
