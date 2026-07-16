#pragma once

#include <QQuickPaintedItem>
#include <QString>

// Pinta una imagen de scope (waveform / vectorscopio) provista por ScopesProvider.
// Los scopes son pequeños y se actualizan a ~15 Hz, así que el pintado por CPU basta.
class ScopeView : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QObject *provider READ provider WRITE setProvider NOTIFY providerChanged)
    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY kindChanged) // "waveform"|"vectorscope"
public:
    explicit ScopeView(QQuickItem *parent = nullptr);

    QObject *provider() const { return m_provider; }
    void setProvider(QObject *provider);
    QString kind() const { return m_kind; }
    void setKind(const QString &kind);

    void paint(QPainter *painter) override;

signals:
    void providerChanged();
    void kindChanged();

private:
    QObject *m_provider = nullptr;
    QString m_kind;
};
