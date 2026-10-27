#version 320 es
// Variantes: -DINSTANCED (matriz por instância) ou -DSKINNED (paleta de joints, sem instancing)
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent;
layout(location = 4) in uvec4 aJoints;
layout(location = 5) in vec4 aWeights;
layout(location = 6) in mat4 aInstance;  // ocupa locations 6..9

layout(std140, binding = 1) uniform JointBlock { mat4 uJoints[128]; };

uniform mat4 uModel;  // usado quando não é INSTANCED

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vTangent;    // xyz tangente, w sinal da bitangente
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
    // inverse-transpose: normais corretas mesmo com escala não uniforme
    mat3 m3 = mat3(model);
    mat3 nm = transpose(inverse(m3));
    vWorldPos = wp.xyz;
    vNormal = normalize(nm * aNormal);
    vTangent = vec4(normalize(m3 * aTangent.xyz), aTangent.w);
    vUV = aUV;
    gl_Position = uViewProj * wp;
    vViewDepth = gl_Position.w;  // profundidade de view para escolher a cascata
}
