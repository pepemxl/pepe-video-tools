#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float sat;
    float nv12;   // >0.5: uTex es el plano UV intercalado (R8G8); vTex no se usa
    float pad;
    vec4 yuvA;    // yScale, yOff, rV, gU
    vec4 yuvB;    // gV, bU, 0, 0
    vec4 off;
    vec4 liftv;
    vec4 gainv;
    vec4 invGam;
} ubuf;

layout(binding = 1) uniform sampler2D yTex;
layout(binding = 2) uniform sampler2D uTex;
layout(binding = 3) uniform sampler2D vTex;

// Capa de vídeo YUV 4:2:0 del PROGRAMA: conversión YUV→RGB + corrección primaria
// (réplica de CompositorWorker::gradeImage) en un solo paso de shader.
void main()
{
    float Y = (texture(yTex, vTexCoord).r - ubuf.yuvA.y) * ubuf.yuvA.x;
    float U, V;
    if (ubuf.nv12 > 0.5) {
        vec2 c = texture(uTex, vTexCoord).rg;
        U = c.r - 0.5;
        V = c.g - 0.5;
    } else {
        U = texture(uTex, vTexCoord).r - 0.5;
        V = texture(vTex, vTexCoord).r - 0.5;
    }
    vec3 v = clamp(vec3(Y + ubuf.yuvA.z * V,
                        Y + ubuf.yuvA.w * U + ubuf.yuvB.x * V,
                        Y + ubuf.yuvB.y * U), 0.0, 1.0);
    v = v + ubuf.off.rgb;
    v = v + ubuf.liftv.rgb * (vec3(1.0) - v);
    v = clamp(v * ubuf.gainv.rgb, 0.0, 1.0);
    v = pow(v, ubuf.invGam.rgb);
    float L = dot(v, vec3(0.299, 0.587, 0.114));
    v = clamp(vec3(L) + (v - vec3(L)) * ubuf.sat, 0.0, 1.0);
    fragColor = vec4(v, 1.0) * ubuf.qt_Opacity;
}
