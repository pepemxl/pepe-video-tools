#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float sat;
    float pad1;
    float pad2;
    vec4 off;
    vec4 liftv;
    vec4 gainv;
    vec4 invGam;
} ubuf;

layout(binding = 1) uniform sampler2D src;

// Réplica del etalonaje por CPU (CompositorWorker::gradeImage): offset de
// temp/tinte, lift en sombras, gain en altas, gamma en medios y saturación
// alrededor de la luma Rec.601. La salida es premultiplicada (la entrada de
// los tiles con alfa ya llega premultiplicada y con etalonaje identidad).
void main()
{
    vec4 t = texture(src, vTexCoord);
    vec3 v = t.rgb + ubuf.off.rgb;
    v = v + ubuf.liftv.rgb * (vec3(1.0) - v);
    v = clamp(v * ubuf.gainv.rgb, 0.0, 1.0);
    v = pow(v, ubuf.invGam.rgb);
    float L = dot(v, vec3(0.299, 0.587, 0.114));
    v = clamp(vec3(L) + (v - vec3(L)) * ubuf.sat, 0.0, 1.0);
    fragColor = vec4(v, t.a) * ubuf.qt_Opacity;
}
