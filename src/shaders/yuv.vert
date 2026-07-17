#version 440

layout(location = 0) in vec4 vertexCoord;
layout(location = 1) in vec2 texCoord;

layout(location = 0) out vec2 vTexCoord;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    // Coeficientes YUV->RGB elegidos por la CPU (BT.601/709, rango limitado/completo)
    float yScale;
    float yOff;
    float rV;
    float gU;
    float gV;
    float bU;
    float nv12;
} ubuf;

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    vTexCoord = texCoord;
    gl_Position = ubuf.qt_Matrix * vertexCoord;
}
