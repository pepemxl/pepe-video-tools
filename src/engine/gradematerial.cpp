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

void GradeMaterial::setColor(const TimelineModel::Color &c)
{
    // Rueda (x,y) → ajuste por canal (misma convención que gradeImage).
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
    sat = float(c.sat);
}
