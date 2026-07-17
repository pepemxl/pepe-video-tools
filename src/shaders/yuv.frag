#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float yScale;
    float yOff;
    float rV;
    float gU;
    float gV;
    float bU;
    float nv12;   // >0.5: uTex es el plano UV intercalado (R8G8); vTex no se usa
} ubuf;

layout(binding = 1) uniform sampler2D yTex;
layout(binding = 2) uniform sampler2D uTex;
layout(binding = 3) uniform sampler2D vTex;

void main()
{
    float Y = (texture(yTex, vTexCoord).r - ubuf.yOff) * ubuf.yScale;
    float U, V;
    if (ubuf.nv12 > 0.5) {
        vec2 c = texture(uTex, vTexCoord).rg;
        U = c.r - 0.5;
        V = c.g - 0.5;
    } else {
        U = texture(uTex, vTexCoord).r - 0.5;
        V = texture(vTex, vTexCoord).r - 0.5;
    }
    vec3 rgb = vec3(Y + ubuf.rV * V,
                    Y + ubuf.gU * U + ubuf.gV * V,
                    Y + ubuf.bU * U);
    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0) * ubuf.qt_Opacity;
}
