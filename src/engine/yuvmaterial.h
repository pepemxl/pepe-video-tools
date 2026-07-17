#pragma once

#include <QSGMaterial>
#include <QSGTexture>
#include <memory>

// Material del scene graph que compone un fotograma YUV 4:2:0 haciendo la conversión
// YUV→RGB en el fragment shader (src/shaders/yuv.*). Dos modos:
//  - I420: tres texturas R8 (Y, U, V) subidas desde RAM (1.5 B/px en vez de 4).
//  - NV12 zero-copy: dos SRVs (R8 = plano Y, R8G8 = plano UV) sobre una textura
//    NV12 D3D11 que ya vive en el dispositivo del scene graph — el material retiene
//    los QRhiTexture envolventes y una referencia a la textura nativa mientras se usan.
class YuvMaterial : public QSGMaterial
{
public:
    YuvMaterial();
    ~YuvMaterial() override;   // libera texturas, envoltorios y referencia nativa

    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial *other) const override;

    // Sustituye las texturas de los planos (libera las anteriores; admite punteros
    // repetidos — p. ej. NV12 usa la misma textura UV en los slots 1 y 2). En el modo
    // zero-copy las QSGTexture poseen sus envoltorios QRhiTexture (los destruyen al
    // liberarse) y `nativeRef` mantiene viva la textura NV12 nativa mientras se usa.
    void setTextures(QSGTexture *y, QSGTexture *u, QSGTexture *v,
                     std::shared_ptr<void> nativeRef = {});
    QSGTexture *texture(int i) const { return m_tex[i]; }

    // Elige la matriz YUV→RGB según el espacio de color y rango de FFmpeg
    // (AVCOL_SPC_* / AVCOL_RANGE_*). Sin especificar: BT.709 si la imagen es HD
    // (alto >= 720), BT.601 en otro caso — el mismo criterio que aplica swscale.
    void setColorimetry(int avColorSpace, int avColorRange, int height);

    // Coeficientes que consume el shader (rango limitado BT.709 por defecto).
    float yScale = 255.0f / 219.0f;
    float yOff = 16.0f / 255.0f;
    float rV = 1.5748f;
    float gU = -0.1873f;
    float gV = -0.4681f;
    float bU = 1.8556f;
    bool nv12 = false;   // uTex = plano UV intercalado (R8G8)

private:
    QSGTexture *m_tex[3] = { nullptr, nullptr, nullptr };   // Y, U, V (V puede == U)
    std::shared_ptr<void> m_nativeRef;                       // textura NV12 nativa
};
