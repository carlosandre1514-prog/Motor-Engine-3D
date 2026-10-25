#version 320 es
// Variantes: -DINSTANCED (matriz por instância) ou -DSKINNED (paleta de joints, sem instancing)
precision highp float;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent;
layout(location = 4) in uvec4 aJoints;
layout(location = 5) in vec4 aWeights;
layout(location = 6) in mat4 aInstance;  // ocupa locations 6..9

layout(std140, binding = 0) uniform SceneBlock {
    mat4 uViewProj;
    vec4 uCamPos;
    vec4 uDirDir;
    vec4 uDirColor;
    mat4 uCascadeVP[3];
    vec4 uCascadeSplits;
    vec4 uSkyAmbient;
    vec4 uGroundAmbient;
    ivec4 uCounts;
    vec4 uPointPosRadius[8];
    vec4 uPointColorIntensity[8];
};

layout(std140, binding = 1) uniform JointBlock { mat4 uJoints[128]; };

uniform mat4 uModel;  // usado quando não é INSTANCED

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec3 vTangent;
out vec3 vBitangent;
out float vViewDepth;

void main() {
#if defined(SKINNED)
    mat4 skin = aWeights.x * uJoints[aJoints.x] + aWeights.y * uJoints[aJoints.y] +
                aWeights.z * uJoints[aJoints.z] + aWeights.w * uJoints[aJoints.w];
    mat4 model = uModel * skin;
#elif defined(INSTANCED)
    mat4 model = aInstance;
#else
    mat4 model = uModel;
#endif
    vec4 wp = model * vec4(aPos, 1.0);
    mat3 nm = mat3(model);  // assume escala uniforme; para não uniforme usar inverse-transpose
    vec3 N = normalize(nm * aNormal);
    vec3 T = normalize(nm * aTangent.xyz);
    vWorldPos = wp.xyz;
    vNormal = N;
    vTangent = T;
    vBitangent = cross(N, T) * aTangent.w;
    vUV = aUV;
    gl_Position = uViewProj * wp;
    vViewDepth = gl_Position.w;  // profundidade de view para escolher a cascata
}
