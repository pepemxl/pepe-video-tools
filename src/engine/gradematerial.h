#pragma once

#include <QSGMaterial>
#include <QSGTexture>
#include <memory>

#include "../app/timelinemodel.h"   // TimelineModel::Color

// Material del scene graph que dibuja una capa del PROGRAMA aplicando la
// corrección primaria (lift/gamma/gain + temp/tinte/saturación) en el fragment
// shader (src/shaders/grade.*) — réplica exacta del gradeImage por CPU. La
// opacidad de la capa llega por el árbol (QSGOpacityNode → qt_Opacity).
class GradeMaterial : public QSGMaterial
{
public:
    GradeMaterial();
    ~GradeMaterial() override;   // libera la textura

    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial *other) const override;

    void setTexture(QSGTexture *t);   // toma la propiedad (libera la anterior)
    QSGTexture *texture() const { return m_tex; }

    // Calcula los coeficientes del shader desde la corrección del clip
    // (misma matemática que CompositorWorker::gradeImage). Identidad por defecto.
    void setColor(const TimelineModel::Color &c);

    float sat = 1.0f;
    float off[3] = { 0.0f, 0.0f, 0.0f };
    float lift[3] = { 0.0f, 0.0f, 0.0f };
    float gain[3] = { 1.0f, 1.0f, 1.0f };
    float invGam[3] = { 1.0f, 1.0f, 1.0f };

private:
    QSGTexture *m_tex = nullptr;
};

// Variante YUV del material de etalonaje: la capa llega como planos I420 (tres
// texturas R8) o como NV12 zero-copy (SRVs R8 + R8G8 sobre la textura nativa);
// el shader (src/shaders/gradeyuv.*) convierte YUV→RGB y aplica la corrección
// primaria en un solo paso. Ver YuvMaterial para las reglas de texturas/NV12.
class GradeYuvMaterial : public QSGMaterial
{
public:
    GradeYuvMaterial();
    ~GradeYuvMaterial() override;

    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override;
    int compare(const QSGMaterial *other) const override;

    // Igual que YuvMaterial::setTextures (slots repetidos permitidos; en NV12 las
    // QSGTexture poseen sus QRhiTexture y `nativeRef` retiene la textura nativa).
    void setTextures(QSGTexture *y, QSGTexture *u, QSGTexture *v,
                     std::shared_ptr<void> nativeRef = {});
    QSGTexture *texture(int i) const { return m_tex[i]; }

    void setColorimetry(int avColorSpace, int avColorRange, int height);
    void setColor(const TimelineModel::Color &c);   // misma matemática que GradeMaterial

    bool nv12 = false;
    // YUV→RGB (rango limitado BT.709 por defecto).
    float yScale = 255.0f / 219.0f;
    float yOff = 16.0f / 255.0f;
    float rV = 1.5748f;
    float gU = -0.1873f;
    float gV = -0.4681f;
    float bU = 1.8556f;
    // Corrección primaria (identidad por defecto).
    float sat = 1.0f;
    float off[3] = { 0.0f, 0.0f, 0.0f };
    float lift[3] = { 0.0f, 0.0f, 0.0f };
    float gain[3] = { 1.0f, 1.0f, 1.0f };
    float invGam[3] = { 1.0f, 1.0f, 1.0f };

private:
    QSGTexture *m_tex[3] = { nullptr, nullptr, nullptr };
    std::shared_ptr<void> m_nativeRef;
};
