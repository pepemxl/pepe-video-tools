#pragma once

#include <QSGMaterial>
#include <QSGTexture>

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
