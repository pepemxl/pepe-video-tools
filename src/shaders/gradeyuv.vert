#version 440

layout(location = 0) in vec4 vertexCoord;
layout(location = 1) in vec2 texCoord;

layout(location = 0) out vec2 vTexCoord;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float sat;
    float nv12;
    float pad;
    vec4 yuvA;     // yScale, yOff, rV, gU
    vec4 yuvB;     // gV, bU, 0, 0
    vec4 off;      // temp/tinte por canal
    vec4 liftv;
    vec4 gainv;
    vec4 invGam;
} ubuf;

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    vTexCoord = texCoord;
    gl_Position = ubuf.qt_Matrix * vertexCoord;
}
