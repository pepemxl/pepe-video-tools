#include "yuvmaterial.h"

#include <QSGMaterialShader>
#include <rhi/qrhi.h>
#include <cstring>

// Espacios de color de FFmpeg que necesitamos distinguir (valores de AVColorSpace /
// AVColorRange en libavutil/pixfmt.h; copiados para no arrastrar los headers aquí).
static constexpr int kSpcBt709 = 1;
static constexpr int kSpcUnspecified = 2;
static constexpr int kRangeJpeg = 2;   // rango completo (0..255)

class YuvShader : public QSGMaterialShader
{
public:
    YuvShader()
    {
        setShaderFileName(VertexStage, QLatin1String(":/pvs/shaders/yuv.vert.qsb"));
        setShaderFileName(FragmentStage, QLatin1String(":/pvs/shaders/yuv.frag.qsb"));
    }

    bool updateUniformData(RenderState &state, QSGMaterial *newMaterial, QSGMaterial *) override
    {
        QByteArray *buf = state.uniformData();
        if (state.isMatrixDirty())
            std::memcpy(buf->data(), state.combinedMatrix().constData(), 64);
        if (state.isOpacityDirty()) {
            const float o = state.opacity();
            std::memcpy(buf->data() + 64, &o, 4);
        }
        const auto *m = static_cast<YuvMaterial *>(newMaterial);
        const float c[7] = { m->yScale, m->yOff, m->rV, m->gU, m->gV, m->bU,
                             m->nv12 ? 1.0f : 0.0f };
        std::memcpy(buf->data() + 68, c, sizeof c);
        return true;
    }

    void updateSampledImage(RenderState &state, int binding, QSGTexture **texture,
                            QSGMaterial *newMaterial, QSGMaterial *) override
    {
        auto *m = static_cast<YuvMaterial *>(newMaterial);
        if (binding < 1 || binding > 3)
            return;
        if (QSGTexture *t = m->texture(binding - 1)) {
            t->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
            *texture = t;
        }
    }
};

YuvMaterial::YuvMaterial()
{
    setFlag(Blending, true);   // respeta la opacidad heredada del árbol de ítems
}

YuvMaterial::~YuvMaterial()
{
    setTextures(nullptr, nullptr, nullptr);
}

QSGMaterialType *YuvMaterial::type() const
{
    static QSGMaterialType t;
    return &t;
}

QSGMaterialShader *YuvMaterial::createShader(QSGRendererInterface::RenderMode) const
{
    return new YuvShader;
}

int YuvMaterial::compare(const QSGMaterial *other) const
{
    const auto *m = static_cast<const YuvMaterial *>(other);
    for (int i = 0; i < 3; ++i)
        if (m_tex[i] != m->m_tex[i])
            return m_tex[i] < m->m_tex[i] ? -1 : 1;
    return 0;
}

void YuvMaterial::setTextures(QSGTexture *y, QSGTexture *u, QSGTexture *v,
                              std::shared_ptr<void> nativeRef)
{
    QSGTexture *incoming[3] = { y, u, v };
    // Libera cada textura anterior una sola vez (los slots pueden repetir puntero)
    // y solo si no sigue en uso en la nueva asignación.
    for (int i = 0; i < 3; ++i) {
        QSGTexture *old = m_tex[i];
        if (!old)
            continue;
        bool seen = false;
        for (int j = 0; j < i; ++j)
            if (m_tex[j] == old) { seen = true; break; }
        bool kept = false;
        for (QSGTexture *in : incoming)
            if (in == old) { kept = true; break; }
        if (!seen && !kept)
            delete old;
    }
    for (int i = 0; i < 3; ++i)
        m_tex[i] = incoming[i];

    // La referencia nativa se suelta después de liberar las QSGTexture que la usaban.
    m_nativeRef = std::move(nativeRef);
}

void YuvMaterial::setColorimetry(int avColorSpace, int avColorRange, int height)
{
    const bool fullRange = avColorRange == kRangeJpeg;
    bool bt709 = avColorSpace == kSpcBt709;
    if (avColorSpace == kSpcUnspecified || avColorSpace == 0)
        bt709 = height >= 720;

    yScale = fullRange ? 1.0f : 255.0f / 219.0f;
    yOff = fullRange ? 0.0f : 16.0f / 255.0f;
    // Coeficientes de rango completo; el escalado de croma limitado (224/255) se
    // integra multiplicando por 255/224.
    const float cs = fullRange ? 1.0f : 255.0f / 224.0f;
    if (bt709) {
        rV = 1.5748f * cs;
        gU = -0.18732f * cs;
        gV = -0.46812f * cs;
        bU = 1.8556f * cs;
    } else {                    // BT.601
        rV = 1.402f * cs;
        gU = -0.344136f * cs;
        gV = -0.714136f * cs;
        bU = 1.772f * cs;
    }
}
