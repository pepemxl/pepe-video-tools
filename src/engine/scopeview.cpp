#include "scopeview.h"
#include "scopesprovider.h"

#include <QPainter>

ScopeView::ScopeView(QQuickItem *parent) : QQuickPaintedItem(parent) {}

void ScopeView::setProvider(QObject *provider)
{
    if (m_provider == provider)
        return;
    if (m_provider)
        disconnect(m_provider, nullptr, this, nullptr);
    m_provider = provider;
    if (auto *sp = qobject_cast<ScopesProvider *>(m_provider))
        connect(sp, &ScopesProvider::scopesUpdated, this, [this]() { update(); });
    emit providerChanged();
    update();
}

void ScopeView::setKind(const QString &kind)
{
    if (m_kind == kind)
        return;
    m_kind = kind;
    emit kindChanged();
    update();
}

void ScopeView::paint(QPainter *painter)
{
    auto *sp = qobject_cast<ScopesProvider *>(m_provider);
    if (!sp)
        return;
    const QImage img = sp->scopeImage(m_kind);
    if (img.isNull())
        return;
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(QRectF(0, 0, width(), height()), img);
}
