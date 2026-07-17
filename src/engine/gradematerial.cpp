#include "gradematerial.h"

#include <QSGMaterialShader>
#include <cstring>

class GradeShader : public QSGMaterialShader
{
public:
    GradeShader()
    {
        setShaderFileName(VertexStage, QLatin1String(":/pvs/shaders/grade.vert.qsb"));
        setShaderFileName(FragmentStage, QLatin1String(":/pvs/shaders/grade.frag.qsb"));
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
        const auto *m = static_cast<GradeMaterial *>(newMaterial);
        std::memcpy(buf->data() + 68, &m->sat, 4);
        const float off4[4] = { m->off[0], m->off[1], m->off[2], 0.0f };
        const float lift4[4] = { m->lift[0], m->lift[1], m->lift[2], 0.0f };
        const float gain4[4] = { m->gain[0], m->gain[1], m->gain[2], 1.0f };
        const float gam4[4] = { m->invGam[0], m->invGam[1], m->invGam[2], 1.0f };
        std::memcpy(buf->data() + 80, off4, 16);
        std::memcpy(buf->data() + 96, lift4, 16);
        std::memcpy(buf->data() + 112, gain4, 16);
        std::memcpy(buf->data() + 128, gam4, 16);
        return true;
    }

    void updateSampledImage(RenderState &state, int binding, QSGTexture **texture,
                            QSGMaterial *newMaterial, QSGMaterial *) override
    {
        auto *m = static_cast<GradeMaterial *>(newMaterial);
        if (binding != 1)
            return;
        if (QSGTexture *t = m->texture()) {
            t->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
            *texture = t;
        }
    }
};

GradeMaterial::GradeMaterial()
{
    setFlag(Blending, true);   // capas con opacidad y tiles con alfa (títulos)
}

GradeMaterial::~GradeMaterial()
{
    setTexture(nullptr);
}

QSGMaterialType *GradeMaterial::type() const
{
    static QSGMaterialType t;
    return &t;
}

QSGMaterialShader *GradeMaterial::createShader(QSGRendererInterface::RenderMode) const
{
    return new GradeShader;
}

int GradeMaterial::compare(const QSGMaterial *other) const
{
    const auto *m = static_cast<const GradeMaterial *>(other);
    if (m_tex != m->m_tex)
        return m_tex < m->m_tex ? -1 : 1;
    const int c = std::memcmp(off, m->off, sizeof off);
    if (c) return c;
    if (sat != m->sat) return sat < m->sat ? -1 : 1;
    const int l = std::memcmp(lift, m->lift, sizeof lift);
    if (l) return l;
    const int g = std::memcmp(gain, m->gain, sizeof gain);
    if (g) return g;
    return std::memcmp(invGam, m->invGam, sizeof invGam);
}

void GradeMaterial::setTexture(QSGTexture *t)
{
    if (m_tex && m_tex != t)
        delete m_tex;
    m_tex = t;
}

// Coeficientes compartidos por ambos materiales (réplica de gradeImage).
static void gradeCoefficients(const TimelineModel::Color &c, float *off, float *lift,
                              float *gain, float *invGam, float *sat)
{
    auto wheel = [](double x, double y, double &r, double &g, double &b) {
        r = y;
        g = -0.866 * x - 0.5 * y;
        b = 0.866 * x - 0.5 * y;
    };
    double lR, lG, lB, gR, gG, gB, mR, mG, mB;
    wheel(c.liftX, c.liftY, lR, lG, lB);
    wheel(c.gainX, c.gainY, gR, gG, gB);
    wheel(c.gammaX, c.gammaY, mR, mG, mB);

    off[0] = float(c.temp * 0.2);
    off[1] = float(c.tint * 0.2);
    off[2] = float(-c.temp * 0.2);
    const double lifts[3] = { lR, lG, lB };
    const double gains[3] = { gR, gG, gB };
    const double gams[3] = { mR, mG, mB };
    for (int ch = 0; ch < 3; ++ch) {
        lift[ch] = float(lifts[ch] * 0.3);
        gain[ch] = float(1.0 + gains[ch] * 0.5);
        invGam[ch] = float(1.0 / qMax(0.1, 1.0 + gams[ch] * 0.5));
    }
    *sat = float(c.sat);
}

// ===================== GradeYuvMaterial =====================

class GradeYuvShader : public QSGMaterialShader
{
public:
    GradeYuvShader()
    {
        setShaderFileName(VertexStage, QLatin1String(":/pvs/shaders/gradeyuv.vert.qsb"));
        setShaderFileName(FragmentStage, QLatin1String(":/pvs/shaders/gradeyuv.frag.qsb"));
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
        const auto *m = static_cast<GradeYuvMaterial *>(newMaterial);
        std::memcpy(buf->data() + 68, &m->sat, 4);
        const float nv = m->nv12 ? 1.0f : 0.0f;
        std::memcpy(buf->data() + 72, &nv, 4);
        const float yuvA[4] = { m->yScale, m->yOff, m->rV, m->gU };
        const float yuvB[4] = { m->gV, m->bU, 0.0f, 0.0f };
        const float off4[4] = { m->off[0], m->off[1], m->off[2], 0.0f };
        const float lift4[4] = { m->lift[0], m->lift[1], m->lift[2], 0.0f };
        const float gain4[4] = { m->gain[0], m->gain[1], m->gain[2], 1.0f };
        const float gam4[4] = { m->invGam[0], m->invGam[1], m->invGam[2], 1.0f };
        std::memcpy(buf->data() + 80, yuvA, 16);
        std::memcpy(buf->data() + 96, yuvB, 16);
        std::memcpy(buf->data() + 112, off4, 16);
        std::memcpy(buf->data() + 128, lift4, 16);
        std::memcpy(buf->data() + 144, gain4, 16);
        std::memcpy(buf->data() + 160, gam4, 16);
        return true;
    }

    void updateSampledImage(RenderState &state, int binding, QSGTexture **texture,
                            QSGMaterial *newMaterial, QSGMaterial *) override
    {
        auto *m = static_cast<GradeYuvMaterial *>(newMaterial);
        if (binding < 1 || binding > 3)
            return;
        if (QSGTexture *t = m->texture(binding - 1)) {
            t->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
            *texture = t;
        }
    }
};

GradeYuvMaterial::GradeYuvMaterial()
{
    setFlag(Blending, true);
}

GradeYuvMaterial::~GradeYuvMaterial()
{
    setTextures(nullptr, nullptr, nullptr);
}

QSGMaterialType *GradeYuvMaterial::type() const
{
    static QSGMaterialType t;
    return &t;
}

QSGMaterialShader *GradeYuvMaterial::createShader(QSGRendererInterface::RenderMode) const
{
    return new GradeYuvShader;
}

int GradeYuvMaterial::compare(const QSGMaterial *other) const
{
    const auto *m = static_cast<const GradeYuvMaterial *>(other);
    for (int i = 0; i < 3; ++i)
        if (m_tex[i] != m->m_tex[i])
            return m_tex[i] < m->m_tex[i] ? -1 : 1;
    if (sat != m->sat) return sat < m->sat ? -1 : 1;
    const int c = std::memcmp(off, m->off, sizeof off);
    if (c) return c;
    const int l = std::memcmp(lift, m->lift, sizeof lift);
    if (l) return l;
    const int g = std::memcmp(gain, m->gain, sizeof gain);
    if (g) return g;
    return std::memcmp(invGam, m->invGam, sizeof invGam);
}

void GradeYuvMaterial::setTextures(QSGTexture *y, QSGTexture *u, QSGTexture *v,
                                   std::shared_ptr<void> nativeRef)
{
    QSGTexture *incoming[3] = { y, u, v };
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
    m_nativeRef = std::move(nativeRef);
}

void GradeYuvMaterial::setColorimetry(int avColorSpace, int avColorRange, int height)
{
    static constexpr int kSpcBt709 = 1;
    static constexpr int kSpcUnspecified = 2;
    static constexpr int kRangeJpeg = 2;
    const bool fullRange = avColorRange == kRangeJpeg;
    bool bt709 = avColorSpace == kSpcBt709;
    if (avColorSpace == kSpcUnspecified || avColorSpace == 0)
        bt709 = height >= 720;

    yScale = fullRange ? 1.0f : 255.0f / 219.0f;
    yOff = fullRange ? 0.0f : 16.0f / 255.0f;
    const float cs = fullRange ? 1.0f : 255.0f / 224.0f;
    if (bt709) {
        rV = 1.5748f * cs;
        gU = -0.18732f * cs;
        gV = -0.46812f * cs;
        bU = 1.8556f * cs;
    } else {
        rV = 1.402f * cs;
        gU = -0.344136f * cs;
        gV = -0.714136f * cs;
        bU = 1.772f * cs;
    }
}

void GradeYuvMaterial::setColor(const TimelineModel::Color &c)
{
    gradeCoefficients(c, off, lift, gain, invGam, &sat);
}

void GradeMaterial::setColor(const TimelineModel::Color &c)
{
    gradeCoefficients(c, off, lift, gain, invGam, &sat);
}
