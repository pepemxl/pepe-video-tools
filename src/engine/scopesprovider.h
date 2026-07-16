#pragma once

#include <QImage>
#include <QObject>
#include <QVariantList>

class QTimer;

// Calcula scopes (histograma RGB, waveform de luma y vectorscopio) a partir del fotograma
// del monitor de PROGRAMA. El cálculo se limita en frecuencia (throttle) y submuestrea los
// píxeles para no cargar el hilo de UI. Waveform y vectorscopio se entregan como QImage.
class ScopesProvider : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList histogram READ histogram NOTIFY scopesUpdated)
public:
    explicit ScopesProvider(QObject *parent = nullptr);

    // Conecta a una fuente que emita frameReady(QImage) (el Compositor).
    void setSource(QObject *frameSource);

    QVariantList histogram() const { return m_histogram; }

    // Para ScopeView (y pruebas).
    Q_INVOKABLE QImage scopeImage(const QString &kind) const; // "waveform" | "vectorscope"

public slots:
    void onFrame(const QImage &image);

signals:
    void scopesUpdated();

private:
    void compute();

    QImage m_frame;
    QImage m_waveform;
    QImage m_vectorscope;
    QVariantList m_histogram;
    QTimer *m_throttle = nullptr;
    bool m_dirty = false;
};
