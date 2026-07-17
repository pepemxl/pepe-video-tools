#pragma once

#include <QByteArray>
#include <QImage>
#include <QMetaType>
#include <memory>

// Un fotograma decodificado camino de la GPU, en orden de preferencia:
//
//  1. **Zero-copy** (`native`): textura NV12 D3D11 que ya vive en el dispositivo
//     del scene graph — el fotograma nunca pasa por RAM; la VideoSurface la
//     muestrea con SRVs R8 (Y) y R8G8 (UV) y convierte a RGB en el shader.
//     `texW/texH` es el tamaño real de la textura (alineado por el decoder);
//     `width/height` el área visible.
//  2. Planos YUV 4:2:0 en RAM (I420), que la VideoSurface sube como tres
//     texturas R8 — sin swscale ni QImage RGBA intermedia.
//  3. `rgba`: reserva para formatos de píxel exóticos (ruta clásica por CPU).
struct VideoFrame {
    int width = 0, height = 0;

    // Zero-copy: ID3D11Texture2D* (NV12) con Release() como deleter.
    std::shared_ptr<void> native;
    int texW = 0, texH = 0;

    // Planos I420 empaquetados (stride = width y width/2 respectivamente).
    QByteArray y, u, v;

    // Colorimetría de origen (AVCOL_SPC_* / AVCOL_RANGE_*) para elegir la matriz
    // YUV→RGB del shader. 0 = sin especificar (se decide por la resolución).
    int colorSpace = 0;
    int colorRange = 0;

    // Ruta de reserva: fotograma ya convertido a RGBA por CPU (swscale).
    QImage rgba;

    bool hasNative() const { return native && width > 0 && height > 0; }
    bool hasYuv() const { return width > 0 && height > 0 && !y.isEmpty(); }
    bool isValid() const { return hasNative() || hasYuv() || !rgba.isNull(); }
};
Q_DECLARE_METATYPE(VideoFrame)
