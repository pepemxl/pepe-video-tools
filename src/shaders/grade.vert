#version 440

layout(location = 0) in vec4 vertexCoord;
layout(location = 1) in vec2 texCoord;

layout(location = 0) out vec2 vTexCoord;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float sat;
    float pad1;
    float pad2;
    vec4 off;      // desplazamiento temp/tinte por canal (rgb)
    vec4 liftv;    // lift por canal
    vec4 gainv;    // gain por canal
    vec4 invGam;   // 1/gamma por canal
} ubuf;

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    vTexCoord = texCoord;
    gl_Position = ubuf.qt_Matrix * vertexCoord;
}
