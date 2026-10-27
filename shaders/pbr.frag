#version 320 es
precision highp sampler2DArrayShadow;
precision highp samplerCube;

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec4 vTangent;
in float vViewDepth;
in vec4 vColor;

uniform sampler2D uAlbedoMap;
uniform sampler2D uNormalMap;
uniform sampler2D uOrmMap;        // glTF metallicRoughness: G = roughness, B = metallic
uniform sampler2D uOcclusionMap;  // glTF occlusion: R = AO
uniform sampler2D uEmissiveMap;
uniform sampler2DArrayShadow uShadowMap;
uniform samplerCube uEnvMap;      // céu capturado com mips (reflexo especular aproximado)

uniform vec4 uAlbedoFactor;
uniform float uRoughnessFactor;
uniform float uMetallicFactor;
uniform vec3 uEmissiveFactor;
uniform float uAlphaCutoff;       // 0 = opaco
uniform float uOcclusionStrength; // 0 = sem textura de AO
uniform int uMode;                // 0 padrão, 1 terreno, 2 água (oceano)
uniform float uTime;              // segundos (ondas da água)

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

// Aproximação analítica do termo split-sum (Karis, "Physically Based Shading on Mobile")
vec3 envBRDFApprox(vec3 F0, float rough, float NoV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

vec3 shIrradiance(vec3 n) {
    vec3 r = uSH[0].rgb * 0.282095
           + uSH[1].rgb * 0.488603 * n.y + uSH[2].rgb * 0.488603 * n.z + uSH[3].rgb * 0.488603 * n.x
           + uSH[4].rgb * 1.092548 * n.x * n.y + uSH[5].rgb * 1.092548 * n.y * n.z
           + uSH[6].rgb * 0.315392 * (3.0 * n.z * n.z - 1.0)
           + uSH[7].rgb * 1.092548 * n.x * n.z + uSH[8].rgb * 0.546274 * (n.x * n.x - n.y * n.y);
    return max(r, vec3(0.0));
}

float shadowFactor(vec3 worldPos, vec3 N, vec3 L) {
    if (uDirDir.w < 0.5 || vViewDepth > uCascadeSplits.w) return 1.0;
    int c = vViewDepth < uCascadeSplits.x ? 0 : (vViewDepth < uCascadeSplits.y ? 1 : 2);
    float texel = uCascadeTexel[c];
    float NdotL = clamp(dot(N, L), 0.0, 1.0);
    // normal offset proporcional ao texel da cascata (maior em superfícies rasantes)
    vec3 p = worldPos + N * texel * (1.0 + 2.0 * (1.0 - NdotL));
    vec4 sc = uCascadeVP[c] * vec4(p, 1.0);
    vec3 uvz = sc.xyz / sc.w * 0.5 + 0.5;
    if (any(lessThan(uvz, vec3(0.0))) || any(greaterThan(uvz, vec3(1.0)))) return 1.0;
    float ref = uvz.z - 0.0004;
    // PCF 3x3 de amostras com comparação bilinear por hardware (= filtro 4x4 suave)
    vec2 ts = vec2(1.0 / float(textureSize(uShadowMap, 0).x));
    float s = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            s += texture(uShadowMap, vec4(uvz.xy + vec2(x, y) * ts * 1.25, float(c), ref));
    s *= 1.0 / 9.0;
    // some suavemente no fim da distância de sombra
    float fade = clamp((uCascadeSplits.w - vViewDepth) / (uCascadeSplits.w * 0.15), 0.0, 1.0);
    return mix(1.0, s, fade);
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

// Água realista (técnica do projeto "ocean" de Jérémy Bouny, MIT): 4 camadas da textura de normais
// andando em velocidades diferentes, brilho do sol, Fresnel e cor de espalhamento. O reflexo vem do céu
// (cubemap do ambiente) em vez de redesenhar a cena espelhada: muito mais leve no celular.
vec3 waterNoise(vec2 uv) {
    float t = uTime;
    vec2 uv0 = uv / 41.0 + vec2(t / 17.0, t / 29.0);
    vec2 uv1 = uv / 47.0 - vec2(t / -19.0, t / 31.0);
    vec2 uv2 = uv / vec2(9.1, 10.3) + vec2(t / 101.0, t / 97.0);
    vec2 uv3 = uv / vec2(13.7, 12.9) - vec2(t / 109.0, t / -113.0);
    vec4 n = texture(uNormalMap, uv0) + texture(uNormalMap, uv1) + texture(uNormalMap, uv2) + texture(uNormalMap, uv3);
    return n.xyz * 0.5 - 1.0;
}

vec3 waterColor() {
    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 noise = waterNoise(vWorldPos.xz);
    vec3 N = normalize(vec3(noise.x, 1.0, noise.y));     // superfície plana + perturbação das ondas
    vec3 toEye = uCamPos.xyz - vWorldPos;
    vec3 V = normalize(toEye);
    if (dot(V, up) < 0.0) N = -N;                          // olhando de baixo d'água
    float sunI = max(max(uDirColor.r, uDirColor.g), uDirColor.b);
    vec3 sun = uDirColor.rgb / max(sunI, 1e-3) * min(sunI, 4.0) * 0.45;
    vec3 L = normalize(-uDirDir.xyz);
    vec3 refl = normalize(reflect(-L, N));
    vec3 specular = pow(max(0.0, dot(V, refl)), 100.0) * sun * 2.0;
    vec3 diffuse = max(dot(L, N), 0.0) * sun * 0.5;
    vec3 R = reflect(-V, N);
    R.y = abs(R.y);                                         // não reflete o "chão" do céu
    vec3 sky = textureLod(uEnvMap, R, 1.0).rgb * uEnvParams.z;
    float theta = max(dot(V, N), 0.0);
    float reflectance = 0.06 + 0.94 * pow(1.0 - theta, 4.0);   // de cima vê a água funda; de lado, o céu
    vec3 scatter = (0.35 + 0.65 * max(0.0, dot(N, V))) * uAlbedoFactor.rgb * (0.5 + shIrradiance(up) * 2.0);
    vec3 c = mix(sun * diffuse * 0.08 + scatter, sky * 0.75 + sky * specular * 0.5, reflectance) + specular * 0.9;
    if (uFog.w > 0.0) c = mix(c, uFog.rgb, 1.0 - exp(-uFog.w * length(toEye)));
    return c;
}

void main() {
    if (uMode == 2) { oColor = vec4(toDisplay(waterColor()), 1.0); return; }
    vec4 albedoS = texture(uAlbedoMap, vUV) * uAlbedoFactor * vColor;  // textura em sRGB: o GL já linearizou
    if (albedoS.a < uAlphaCutoff) discard;
    if (uMode == 1) {
        // Terreno: grama (textura) / rocha nas encostas / areia perto da água / neve no alto
        float slope = 1.0 - normalize(vNormal).y;
        float detail = texture(uAlbedoMap, vUV * 0.21).g;
        vec3 rock = vec3(0.36, 0.34, 0.32) * (0.7 + 0.6 * detail);
        vec3 sand = vec3(0.70, 0.63, 0.45) * (0.85 + 0.3 * detail);
        vec3 snow = vec3(0.90, 0.92, 0.96);
        vec3 c = mix(albedoS.rgb, rock, smoothstep(0.22, 0.42, slope));
        c = mix(sand, c, smoothstep(0.8, 2.2, vWorldPos.y + detail));
        c = mix(c, snow, smoothstep(52.0, 64.0, vWorldPos.y + detail * 8.0) * (1.0 - smoothstep(0.35, 0.6, slope)));
        albedoS.rgb = c;
    }
    vec3 orm = texture(uOrmMap, vUV).rgb;
    float ao = mix(1.0, texture(uOcclusionMap, vUV).r, uOcclusionStrength);
    float rough = clamp(orm.g * uRoughnessFactor, 0.045, 1.0), metal = clamp(orm.b * uMetallicFactor, 0.0, 1.0);

    vec3 Nv = normalize(vNormal);
    if (!gl_FrontFacing) Nv = -Nv;                                // materiais double-sided
    vec3 T = normalize(vTangent.xyz - Nv * dot(Nv, vTangent.xyz));
    vec3 B = cross(Nv, T) * vTangent.w;
    vec3 nt = texture(uNormalMap, vUV).xyz * 2.0 - 1.0;
    vec3 N = normalize(mat3(T, B, Nv) * nt);
    vec3 V = normalize(uCamPos.xyz - vWorldPos);
    vec3 F0 = mix(vec3(0.04), albedoS.rgb, metal);
    float NoV = max(dot(N, V), 1e-4);

    vec3 Lo = vec3(0.0);
    vec3 Ld = normalize(-uDirDir.xyz);
    Lo += brdf(N, V, Ld, uDirColor.rgb, albedoS.rgb, rough, metal, F0) * shadowFactor(vWorldPos, Nv, Ld);

    for (int i = 0; i < uCounts.x; ++i) {
        vec3 d = uPoints[i].posRadius.xyz - vWorldPos;
        float dist = max(length(d), 1e-3), r = uPoints[i].posRadius.w;
        float att = clamp(1.0 - pow(dist / r, 4.0), 0.0, 1.0);   // janela suave (estilo UE4)
        att = att * att / (dist * dist + 1.0);
        Lo += brdf(N, V, d / dist, uPoints[i].colorIntensity.rgb * uPoints[i].colorIntensity.w * att, albedoS.rgb, rough, metal, F0);
    }

    // IBL aproximado: difuso por SH9 do céu + especular do cubemap pré-filtrado por mips
    vec3 R = reflect(-V, N);
    float horizon = clamp(1.0 + dot(R, Nv), 0.0, 1.0);             // evita reflexo "por baixo" da superfície
    vec3 prefiltered = textureLod(uEnvMap, R, rough * uEnvParams.y).rgb * horizon * horizon;
    vec3 diffuseIBL = shIrradiance(N) * albedoS.rgb * (1.0 - metal);
    vec3 specIBL = prefiltered * envBRDFApprox(F0, rough, NoV);
    vec3 ambient = (diffuseIBL + specIBL) * ao * uEnvParams.z;

    vec3 color = Lo + ambient + texture(uEmissiveMap, vUV).rgb * uEmissiveFactor;
    if (uFog.w > 0.0) {
        float fog = 1.0 - exp(-uFog.w * length(uCamPos.xyz - vWorldPos));
        color = mix(color, uFog.rgb, fog);
    }
    oColor = vec4(toDisplay(color), albedoS.a);
}
