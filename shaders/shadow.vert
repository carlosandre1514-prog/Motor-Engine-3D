#version 320 es
layout(location = 0) in vec3 aPos;
layout(location = 4) in uvec4 aJoints;
layout(location = 5) in vec4 aWeights;
layout(location = 6) in mat4 aInstance;

layout(std140, binding = 1) uniform JointBlock { mat4 uJoints[128]; };
uniform mat4 uLightVP;
uniform mat4 uModel;
out vec3 vWorldPos;
flat out uint vCut;

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
    // bits das aberturas que cortam este objeto vêm na linha de baixo da matriz (sempre 0 numa matriz afim)
    vCut = uint(model[0][3] + 0.5) | (uint(model[1][3] + 0.5) << 16);
    model[0][3] = 0.0; model[1][3] = 0.0;
    vec4 wp = model * vec4(aPos, 1.0);
    vWorldPos = wp.xyz;
    gl_Position = uLightVP * wp;
}
