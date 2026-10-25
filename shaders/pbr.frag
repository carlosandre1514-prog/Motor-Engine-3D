#version 320 es
precision highp float;
precision highp sampler2DArrayShadow;

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec3 vTangent;
in vec3 vBitangent;
in float vViewDepth;

layout(std140, binding = 0) uniform SceneBlock {
    mat4 uViewProj;
    vec4 uCamPos;
    vec4 uDirDir;            // w = shadows on/off
    vec4 uDirColor;
    mat4 uCascadeVP[3];
    vec4 uCascadeSplits;
    vec4 uSkyAmbient;
    vec4 uGroundAmbient;
    ivec4 uCounts;
    vec4 uPointPosRadius[8];
    vec4 uPointColorIntensity[8];
};

uniform sampler2D uAlbedoMap;
uniform sampler2D uNormalMap;
uniform sampler2D uOrmMap;        // glTF: R = AO, G = roughness, B = metallic
uniform sampler2D uEmissiveMap;
uniform sampler2DArrayShadow uShadowMap;

uniform vec4 uAlbedoFactor;
uniform float uRoughnessFactor;
uniform float uMetallicFactor;
uniform vec3 uEmissiveFactor;

out vec4 oColor;

const float PI = 3.14159265359;

float distributionGGX(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}
float geometrySmith(float NdotV, float NdotL, float rough) {
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}
vec3 fresnelSchlick(float cosT, vec3 F0) { return F0 + (1.0 - F0) * pow(clamp(1.0 - cosT, 0.0, 1.0), 5.0); }

float shadowFactor(vec3 worldPos, vec3 N, vec3 L) {
    if (uDirDir.w < 0.5) return 1.0;
    int c = vViewDepth < uCascadeSplits.x ? 0 : (vViewDepth < uCascadeSplits.y ? 1 : 2);
    vec3 p = worldPos + N * 0.02 * float(c + 1);                 // normal offset bias
    vec4 sc = uCascadeVP[c] * vec4(p, 1.0);
    vec3 uvz = sc.xyz / sc.w * 0.5 + 0.5;
    if (uvz.z > 1.0 || any(lessThan(uvz.xy, vec2(0.0))) || any(greaterThan(uvz.xy, vec2(1.0)))) return 1.0;
    float bias = max(0.0005 * (1.0 - dot(N, L)), 0.0002);
    return texture(uShadowMap, vec4(uvz.xy, float(c), uvz.z - bias));  // PCF 2x2 por hardware
}

vec3 brdf(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float rough, float metal, vec3 F0) {
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0), NdotV = max(dot(N, V), 1e-4), NdotH = max(dot(N, H), 0.0);
    float D = distributionGGX(NdotH, rough * rough);
    float G = geometrySmith(NdotV, NdotL, rough);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 spec = D * G * F / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kd = (1.0 - F) * (1.0 - metal);
    return (kd * albedo / PI + spec) * radiance * NdotL;
}

vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }

void main() {
    vec4 albedoS = texture(uAlbedoMap, vUV) * uAlbedoFactor;     // textura em sRGB: o GL já linearizou
    vec3 orm = texture(uOrmMap, vUV).rgb;
    float ao = orm.r, rough = clamp(orm.g * uRoughnessFactor, 0.04, 1.0), metal = orm.b * uMetallicFactor;

    vec3 nt = texture(uNormalMap, vUV).xyz * 2.0 - 1.0;
    vec3 N = normalize(mat3(normalize(vTangent), normalize(vBitangent), normalize(vNormal)) * nt);
    vec3 V = normalize(uCamPos.xyz - vWorldPos);
    vec3 F0 = mix(vec3(0.04), albedoS.rgb, metal);

    vec3 Lo = vec3(0.0);
    vec3 Ld = normalize(-uDirDir.xyz);
    Lo += brdf(N, V, Ld, uDirColor.rgb, albedoS.rgb, rough, metal, F0) * shadowFactor(vWorldPos, N, Ld);

    for (int i = 0; i < uCounts.x; ++i) {
        vec3 d = uPointPosRadius[i].xyz - vWorldPos;
        float dist = length(d), r = uPointPosRadius[i].w;
        float att = clamp(1.0 - pow(dist / r, 4.0), 0.0, 1.0);   // janela suave (estilo UE4)
        att = att * att / (dist * dist + 1.0);
        Lo += brdf(N, V, d / dist, uPointColorIntensity[i].rgb * uPointColorIntensity[i].w * att, albedoS.rgb, rough, metal, F0);
    }

    // Ambiente hemisférico (placeholder barato; trocar por IBL pré-filtrado)
    vec3 amb = mix(uGroundAmbient.rgb, uSkyAmbient.rgb, N.y * 0.5 + 0.5) * uSkyAmbient.w;
    vec3 color = Lo + amb * albedoS.rgb * (1.0 - metal) * ao + texture(uEmissiveMap, vUV).rgb * uEmissiveFactor;

    color = aces(color);
    oColor = vec4(pow(color, vec3(1.0 / 2.2)), albedoS.a);       // use framebuffer sRGB se disponível
}
