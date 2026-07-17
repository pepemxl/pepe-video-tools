#include "videosurface.h"
#include "gradematerial.h"
#include "yuvmaterial.h"

#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGOpacityNode>
#include <QSGSimpleRectNode>
#include <QSGTexture>
#include <QSGTransformNode>
#include <rhi/qrhi.h>

VideoSurface::VideoSurface(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

void VideoSurface::setSource(QObject *source)
{
    if (m_source == source)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    m_layersMode = false;
    if (m_source) {
        // Conexión por firma, en orden de preferencia: layersReady(ProgramLayers)
        // (Compositor → composición GPU), frameReady(VideoFrame) (decodificador del
        // ORIGEN, ruta YUV) o frameReady(QImage) (fotograma ya compuesto por CPU).
        bool envOk = false;
        const int gpuEnv = qEnvironmentVariableIntValue("PVS_GPU_PROG", &envOk);
        const bool gpuProg = !envOk || gpuEnv != 0;
        const QMetaObject *mo = m_source->metaObject();
        if (gpuProg && mo->indexOfSignal("layersReady(ProgramLayers)") >= 0) {
            connect(m_source, SIGNAL(layersReady(ProgramLayers)), this, SLOT(onLayers(ProgramLayers)));
            m_layersMode = true;
        } else if (mo->indexOfSignal("frameReady(VideoFrame)") >= 0) {
            connect(m_source, SIGNAL(frameReady(VideoFrame)), this, SLOT(onVideoFrame(VideoFrame)));
        } else {
            connect(m_source, SIGNAL(frameReady(QImage)), this, SLOT(onFrame(QImage)));
        }
    }
    emit sourceChanged();
}

void VideoSurface::setZoom(double zoom)
{
    zoom = qMax(0.0, zoom);
    if (m_zoom == zoom)   // valores discretos del desplegable: comparación exacta
        return;
    m_zoom = zoom;
    emit zoomChanged();
    update();
}

void VideoSurface::setPanX(double v)
{
    if (m_panX == v)
        return;
    m_panX = v;
    emit panChanged();
    update();
}

void VideoSurface::setPanY(double v)
{
    if (m_panY == v)
        return;
    m_panY = v;
    emit panChanged();
    update();
}

void VideoSurface::onFrame(const QImage &image)
{
    m_frame = image;
    m_yuv = VideoFrame();
    m_frameDirty = true;
    update();
}

void VideoSurface::onLayers(const ProgramLayers &layers)
{
    m_layers = layers;
    m_frame = QImage();
    m_yuv = VideoFrame();
    m_frameDirty = true;
    update();
}

void VideoSurface::onVideoFrame(const VideoFrame &frame)
{
    if (frame.hasNative() || frame.hasYuv()) {
        m_yuv = frame;
        m_frame = QImage();
    } else {
        m_frame = frame.rgba;
        m_yuv = VideoFrame();
    }
    m_frameDirty = true;
    update();
}

void VideoSurface::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        update(); // recalcular el letterbox
}

// Ajuste (letterbox KeepAspectRatio) o zoom fijo sobre el tamaño nativo del
// fotograma; con zoom, 1.0 = un píxel de origen por píxel físico (÷ el DPR).
QRectF VideoSurface::targetRect(const QSizeF &frameSize) const
{
    const QSizeF area = size();
    QSizeF scaled;
    if (m_zoom > 0.0) {
        const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1.0;
        scaled = frameSize * (m_zoom / dpr);
    } else {
        scaled = frameSize.scaled(area, Qt::KeepAspectRatio);
    }
    // El paneo solo aplica con zoom (en ajuste el fotograma cabe entero).
    const double px = m_zoom > 0.0 ? m_panX : 0.0;
    const double py = m_zoom > 0.0 ? m_panY : 0.0;
    return QRectF((area.width() - scaled.width()) / 2.0 + px,
                  (area.height() - scaled.height()) / 2.0 + py,
                  scaled.width(), scaled.height());
}

// QImage R8 que comparte el plano sin copiarlo: la función de limpieza retiene una
// referencia al QByteArray mientras la textura lo necesite.
static QImage planeImage(const QByteArray &plane, int w, int h)
{
    auto *keep = new QByteArray(plane);
    return QImage(reinterpret_cast<const uchar *>(keep->constData()), w, h, w,
                  QImage::Format_Grayscale8,
                  [](void *p) { delete static_cast<QByteArray *>(p); }, keep);
}

// Conversión YUV→RGB por CPU, solo para el renderer por software de Qt Quick
// (los materiales personalizados no están soportados ahí).
static QImage yuvToImageCpu(const VideoFrame &f)
{
    if (!f.hasYuv())
        return {};
    QImage out(f.width, f.height, QImage::Format_RGBA8888);
    YuvMaterial m;   // reutiliza la selección de coeficientes
    m.setColorimetry(f.colorSpace, f.colorRange, f.height);
    const int cw = f.width / 2;
    for (int yy = 0; yy < f.height; ++yy) {
        const uchar *py = reinterpret_cast<const uchar *>(f.y.constData()) + qsizetype(yy) * f.width;
        const uchar *pu = reinterpret_cast<const uchar *>(f.u.constData()) + qsizetype(yy / 2) * cw;
        const uchar *pv = reinterpret_cast<const uchar *>(f.v.constData()) + qsizetype(yy / 2) * cw;
        uchar *dst = out.scanLine(yy);
        for (int x = 0; x < f.width; ++x) {
            const float Y = (py[x] / 255.0f - m.yOff) * m.yScale;
            const float U = pu[x / 2] / 255.0f - 0.5f;
            const float V = pv[x / 2] / 255.0f - 0.5f;
            const float r = Y + m.rV * V, g = Y + m.gU * U + m.gV * V, b = Y + m.bU * U;
            dst[4 * x + 0] = uchar(qBound(0.0f, r, 1.0f) * 255.0f + 0.5f);
            dst[4 * x + 1] = uchar(qBound(0.0f, g, 1.0f) * 255.0f + 0.5f);
            dst[4 * x + 2] = uchar(qBound(0.0f, b, 1.0f) * 255.0f + 0.5f);
            dst[4 * x + 3] = 255;
        }
    }
    return out;
}

// Clave de contenido de una capa: si no cambia, las texturas no se re-suben
// (editar transform/color/opacidad en pausa solo actualiza matrices y uniforms).
static QString layerContentKey(const ProgramLayer &layer)
{
    if (layer.isVideoFrame)
        return QStringLiteral("v:%1:%2:%3")
            .arg(layer.rc.mediaPath).arg(layer.rc.sourceUs / 1000)
            .arg(layer.vf.hasNative() ? quintptr(layer.vf.native.get()) : 0);
    if (layer.rc.kind == QLatin1String("title")) {
        const TimelineModel::Title &tt = layer.rc.title;
        const TimelineModel::Transform &t = layer.rc.transform;
        return QStringLiteral("t:%1|%2|%3|%4|%5|%6|%7|%8|%9|%10")
            .arg(tt.text).arg(tt.sizePt).arg(tt.align).arg(tt.color)
            .arg(tt.bar).arg(tt.barColor)
            .arg(t.posX).arg(t.posY).arg(t.rotation).arg(t.scale);
    }
    return QStringLiteral("f:") + layer.rc.fill;
}

// Subárbol de la composición GPU del PROGRAMA. Estructura:
//   QSGTransformNode raíz (lienzo → ítem: letterbox o zoom/paneo)
//     rect negro del lienzo
//     por capa: [QSGClipNode wipe] → QSGOpacityNode → QSGTransformNode → quad
// El quad usa GradeYuvMaterial (NV12 zero-copy o planos I420: conversión YUV→RGB +
// corrección primaria en un paso de shader) o GradeMaterial (RGBA: tiles/rellenos y
// formatos exóticos). Los nodos PERSISTEN entre fotogramas: las texturas solo se
// re-suben si cambia el contenido de la capa; matrices/uniforms se actualizan
// siempre. Con el renderer por software cae a QSGImageNode gradeado por CPU.
QSGNode *VideoSurface::buildProgramNode(QSGNode *oldNode, bool softwareSg)
{
    const QSizeF canvas(m_layers.canvas);
    auto *root = static_cast<QSGTransformNode *>(oldNode);
    if (!root) {
        root = new QSGTransformNode;
        m_layerNodes.clear();
    }

    // Lienzo → ítem (la raíz aplica letterbox/zoom; los hijos viven en px de lienzo).
    const QRectF rect = targetRect(canvas);
    QMatrix4x4 m;
    m.translate(float(rect.x()), float(rect.y()));
    m.scale(float(rect.width() / canvas.width()), float(rect.height() / canvas.height()));
    root->setMatrix(m);

    if (!m_frameDirty)
        return root;
    m_frameDirty = false;

    // ¿Coincide la estructura (nº de capas, tipo de material, wipe) con la actual?
    auto kindOf = [softwareSg](const ProgramLayer &layer) {
        if (softwareSg) return 4;
        if (layer.vf.hasNative()) return 3;
        if (layer.vf.hasYuv()) return 2;
        return 1;
    };
    bool structureOk = m_layerNodes.size() == m_layers.layers.size() && root->firstChild();
    if (structureOk) {
        for (int i = 0; i < m_layers.layers.size(); ++i) {
            const ProgramLayer &layer = m_layers.layers.at(i);
            const bool wiped = layer.rc.wipe >= 0.0 && layer.rc.wipe < 1.0;
            if (m_layerNodes.at(i).matKind != kindOf(layer)
                || (m_layerNodes.at(i).clip != nullptr) != wiped) {
                structureOk = false;
                break;
            }
        }
    }
    if (!structureOk) {
        while (QSGNode *c = root->firstChild()) {
            root->removeChildNode(c);
            delete c;
        }
        m_layerNodes.clear();
        root->appendChildNode(new QSGSimpleRectNode(QRectF(QPointF(0, 0), canvas), Qt::black));
        for (const ProgramLayer &layer : m_layers.layers) {
            LayerNodes ln;
            ln.matKind = kindOf(layer);
            QSGNode *parent = root;
            if (layer.rc.wipe >= 0.0 && layer.rc.wipe < 1.0) {
                ln.clip = new QSGClipNode;
                ln.clip->setIsRectangular(true);
                parent->appendChildNode(ln.clip);
                parent = ln.clip;
            }
            ln.op = new QSGOpacityNode;
            parent->appendChildNode(ln.op);
            ln.xf = new QSGTransformNode;
            ln.op->appendChildNode(ln.xf);
            if (ln.matKind == 4) {
                ln.img = window()->createImageNode();
                ln.img->setOwnsTexture(true);
                ln.img->setFiltering(QSGTexture::Linear);
                ln.xf->appendChildNode(ln.img);
            } else {
                ln.geom = new QSGGeometryNode;
                auto *g = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
                ln.geom->setGeometry(g);
                ln.geom->setFlag(QSGNode::OwnsGeometry);
                if (ln.matKind == 1)
                    ln.geom->setMaterial(new GradeMaterial);
                else
                    ln.geom->setMaterial(new GradeYuvMaterial);
                ln.geom->setFlag(QSGNode::OwnsMaterial);
                ln.xf->appendChildNode(ln.geom);
            }
            m_layerNodes.push_back(ln);
        }
    }

    // Actualización por capa: matrices/uniforms siempre; texturas solo si cambió
    // el contenido (clave). Con la estructura persistente esto es lo único que
    // corre al editar transform/color en pausa.
    for (int i = 0; i < m_layers.layers.size(); ++i) {
        const ProgramLayer &layer = m_layers.layers.at(i);
        LayerNodes &ln = m_layerNodes[i];
        const TimelineModel::Transform &t = layer.rc.transform;
        const VideoFrame &vf = layer.vf;

        if (ln.clip) {
            ln.clip->setClipRect(QRectF(0, 0, canvas.width() * layer.rc.wipe, canvas.height()));
            ln.clip->markDirty(QSGNode::DirtyGeometry);
        }
        ln.op->setOpacity(t.opacity);

        QRectF quad;
        QRectF texRect(0, 0, 1, 1);
        QMatrix4x4 lm;
        if (layer.isVideoFrame) {
            const double fw = vf.width, fh = vf.height;
            const double cw = qMax(0.02, 1.0 - t.cropL - t.cropR);
            const double ch = qMax(0.02, 1.0 - t.cropT - t.cropB);
            // Con textura nativa el área válida es width/height dentro de texW/texH.
            const double sx = vf.hasNative() && vf.texW > 0 ? fw / vf.texW : 1.0;
            const double sy = vf.hasNative() && vf.texH > 0 ? fh / vf.texH : 1.0;
            texRect = QRectF(t.cropL * sx, t.cropT * sy, cw * sx, ch * sy);
            const QSizeF fit = QSizeF(fw * cw, fh * ch).scaled(canvas, Qt::KeepAspectRatio);
            const QSizeF dst(fit.width() * t.scale, fit.height() * t.scale);
            quad = QRectF(-dst.width() / 2, -dst.height() / 2, dst.width(), dst.height());
            lm.translate(float(canvas.width() * (0.5 + t.posX)),
                         float(canvas.height() * (0.5 + t.posY)));
            if (t.rotation != 0.0)
                lm.rotate(float(t.rotation), 0, 0, 1);
        } else {
            // Tile de título o relleno: cubre el lienzo tal cual (paridad con CPU).
            quad = QRectF(QPointF(0, 0), canvas);
        }
        ln.xf->setMatrix(lm);

        const QString key = layerContentKey(layer);
        const bool newContent = key != ln.contentKey;
        ln.contentKey = key;

        if (ln.matKind == 4) {
            QImage img = !vf.rgba.isNull() ? vf.rgba : yuvToImageCpu(vf);
            if (img.isNull())
                continue;
            if (layer.isVideoFrame)
                CompositorWorker::gradeImage(img, layer.rc.color);
            if (newContent)
                ln.img->setTexture(window()->createTextureFromImage(
                    img, QQuickWindow::TextureHasAlphaChannel));
            ln.img->setRect(quad);
            ln.img->setSourceRect(QRectF(texRect.x() * img.width(), texRect.y() * img.height(),
                                         texRect.width() * img.width(),
                                         texRect.height() * img.height()));
            continue;
        }

        QSGGeometry::updateTexturedRectGeometry(ln.geom->geometry(), quad, texRect);
        ln.geom->markDirty(QSGNode::DirtyGeometry);

        if (ln.matKind == 1) {
            auto *mat = static_cast<GradeMaterial *>(ln.geom->material());
            if (newContent) {
                QSGTexture *tex = window()->createTextureFromImage(
                    vf.rgba, QQuickWindow::TextureHasAlphaChannel);
                if (tex)
                    tex->setFiltering(QSGTexture::Linear);
                mat->setTexture(tex);
            }
            mat->setColor(layer.isVideoFrame ? layer.rc.color : TimelineModel::Color{});
        } else {
            auto *mat = static_cast<GradeYuvMaterial *>(ln.geom->material());
            if (newContent) {
                if (ln.matKind == 3) {
                    // NV12 zero-copy: SRVs R8/RG8 sobre la textura nativa.
                    QRhi *rhi = window()->rhi();
                    const quint64 nat = quint64(reinterpret_cast<quintptr>(vf.native.get()));
                    QRhiTexture *ry = rhi->newTexture(QRhiTexture::R8, QSize(vf.texW, vf.texH));
                    QRhiTexture *ruv = rhi->newTexture(QRhiTexture::RG8,
                                                       QSize(vf.texW / 2, vf.texH / 2));
                    if (ry->createFrom({ nat, 0 }) && ruv->createFrom({ nat, 0 })) {
                        QSGTexture *sy = window()->createTextureFromRhiTexture(ry);
                        QSGTexture *suv = window()->createTextureFromRhiTexture(ruv);
                        for (QSGTexture *tx : { sy, suv })
                            if (tx) tx->setFiltering(QSGTexture::Linear);
                        mat->setTextures(sy, suv, suv, vf.native);
                        mat->nv12 = true;
                    } else {
                        delete ry;
                        delete ruv;
                    }
                } else {
                    const int cw = vf.width / 2, chh = vf.height / 2;
                    QSGTexture *ty = window()->createTextureFromImage(planeImage(vf.y, vf.width, vf.height));
                    QSGTexture *tu = window()->createTextureFromImage(planeImage(vf.u, cw, chh));
                    QSGTexture *tv = window()->createTextureFromImage(planeImage(vf.v, cw, chh));
                    for (QSGTexture *tx : { ty, tu, tv })
                        if (tx) tx->setFiltering(QSGTexture::Linear);
                    mat->setTextures(ty, tu, tv);
                    mat->nv12 = false;
                }
            }
            mat->setColorimetry(vf.colorSpace, vf.colorRange, vf.height);
            mat->setColor(layer.isVideoFrame ? layer.rc.color : TimelineModel::Color{});
        }
        ln.geom->markDirty(QSGNode::DirtyMaterial);
    }

    if (qEnvironmentVariableIsSet("PVS_RHI_DEBUG")) {
        static int n = 0;
        ++n;
        if (n <= 3 || n % 30 == 0)
            qInfo("[RHI] PROGRAMA compuesto en GPU #%d (%lld capas, estructura %s)",
                  n, qint64(m_layers.layers.size()), structureOk ? "reutilizada" : "nueva");
    }
    return root;
}

QSGNode *VideoSurface::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    // Handoff del dispositivo D3D11 del scene graph hacia la fuente (una vez): el
    // decodificador lo usa para el decode zero-copy. Solo con el backend D3D11 y
    // si la fuente sabe adoptarlo (VideoController; el Compositor no lo necesita).
    if (!m_devSent && m_source && window()
        && window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Direct3D11
        && m_source->metaObject()->indexOfMethod("adoptGraphicsDevice(void*)") >= 0) {
        if (void *dev = window()->rendererInterface()->getResource(
                window(), QSGRendererInterface::DeviceResource)) {
            QMetaObject::invokeMethod(m_source, "adoptGraphicsDevice",
                                      Qt::QueuedConnection, Q_ARG(void *, dev));
            m_devSent = true;
        }
    }

    const bool softwareSg = window()
        && window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;

    // Modo de capas (PROGRAMA compuesto en GPU).
    if (m_layersMode) {
        if (!m_layers.hasContent || m_layers.layers.isEmpty()
            || !m_layers.canvas.isValid() || width() <= 0 || height() <= 0) {
            delete oldNode;
            m_layerNodes.clear();
            m_nodeKind = 0;
            return nullptr;
        }
        if (oldNode && m_nodeKind != 3) {
            delete oldNode;
            m_layerNodes.clear();
            oldNode = nullptr;
        }
        m_nodeKind = 3;
        return buildProgramNode(oldNode, softwareSg);
    }

    const bool useNative = m_yuv.hasNative() && !softwareSg;
    const bool useYuv = (useNative || m_yuv.hasYuv()) && !softwareSg;
    // Renderer por software: los materiales personalizados no funcionan; convierte por CPU.
    if (m_yuv.hasYuv() && !m_yuv.hasNative() && softwareSg && m_frameDirty)
        m_frame = yuvToImageCpu(m_yuv);

    if ((useYuv ? false : m_frame.isNull()) || width() <= 0 || height() <= 0) {
        delete oldNode;
        m_nodeKind = 0;
        return nullptr;
    }

    const int wantKind = useYuv ? 2 : 1;
    if (oldNode && m_nodeKind != wantKind) {
        delete oldNode;
        oldNode = nullptr;
    }
    m_nodeKind = wantKind;

    if (useYuv) {
        auto *node = static_cast<QSGGeometryNode *>(oldNode);
        YuvMaterial *mat = nullptr;
        if (!node) {
            node = new QSGGeometryNode;
            auto *g = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
            node->setGeometry(g);
            node->setFlag(QSGNode::OwnsGeometry);
            mat = new YuvMaterial;
            node->setMaterial(mat);
            node->setFlag(QSGNode::OwnsMaterial);
        } else {
            mat = static_cast<YuvMaterial *>(node->material());
        }

        if (m_frameDirty || !mat->texture(0)) {
            if (useNative) {
                // Zero-copy: dos SRVs sobre la textura NV12 nativa (R8 = plano Y,
                // R8G8 = plano UV, la forma canónica de muestrear NV12 en D3D11).
                QRhi *rhi = window()->rhi();
                const quint64 nat = quint64(reinterpret_cast<quintptr>(m_yuv.native.get()));
                QRhiTexture *ry = rhi->newTexture(QRhiTexture::R8,
                                                  QSize(m_yuv.texW, m_yuv.texH));
                QRhiTexture *ruv = rhi->newTexture(QRhiTexture::RG8,
                                                   QSize(m_yuv.texW / 2, m_yuv.texH / 2));
                if (!ry->createFrom({ nat, 0 }) || !ruv->createFrom({ nat, 0 })) {
                    // No se pudo envolver la textura nativa: conserva el último
                    // fotograma visible si lo hay; si el nodo está vacío, retíralo.
                    delete ry;
                    delete ruv;
                    m_frameDirty = false;
                    if (!mat->texture(0)) {
                        delete node;
                        m_nodeKind = 0;
                        return nullptr;
                    }
                    return node;
                }
                // Las QSGTexture devueltas poseen su QRhiTexture (lo destruyen al
                // liberarse); la textura NV12 nativa la retiene el material.
                QSGTexture *sy = window()->createTextureFromRhiTexture(ry);
                QSGTexture *suv = window()->createTextureFromRhiTexture(ruv);
                for (QSGTexture *t : { sy, suv })
                    if (t) t->setFiltering(QSGTexture::Linear);
                mat->setTextures(sy, suv, suv, m_yuv.native);
                mat->nv12 = true;
                if (qEnvironmentVariableIsSet("PVS_RHI_DEBUG")) {
                    static int zn = 0;
                    ++zn;
                    if (zn <= 3 || zn % 30 == 0)
                        qInfo("[RHI] NV12 zero-copy #%d (%dx%d, sin paso por RAM)",
                              zn, m_yuv.width, m_yuv.height);
                }
            } else {
                const int cw = m_yuv.width / 2, ch = m_yuv.height / 2;
                QSGTexture *ty = window()->createTextureFromImage(planeImage(m_yuv.y, m_yuv.width, m_yuv.height));
                QSGTexture *tu = window()->createTextureFromImage(planeImage(m_yuv.u, cw, ch));
                QSGTexture *tv = window()->createTextureFromImage(planeImage(m_yuv.v, cw, ch));
                for (QSGTexture *t : { ty, tu, tv })
                    if (t) t->setFiltering(QSGTexture::Linear);
                mat->setTextures(ty, tu, tv);
                mat->nv12 = false;
                if (qEnvironmentVariableIsSet("PVS_RHI_DEBUG")) {
                    static int n = 0;
                    ++n;
                    if (n <= 3 || n % 30 == 0)
                        qInfo("[RHI] planos YUV #%d subidos (%dx%d, conversión en GPU)",
                              n, m_yuv.width, m_yuv.height);
                }
            }
            mat->setColorimetry(m_yuv.colorSpace, m_yuv.colorRange, m_yuv.height);
            node->markDirty(QSGNode::DirtyMaterial);
            m_frameDirty = false;
        }

        // Con textura nativa el tamaño real puede exceder el visible (alineación del
        // decoder): recorta con las coordenadas de textura.
        const QRectF texRect = useNative && m_yuv.texW > 0
            ? QRectF(0, 0, qreal(m_yuv.width) / m_yuv.texW, qreal(m_yuv.height) / m_yuv.texH)
            : QRectF(0, 0, 1, 1);
        QSGGeometry::updateTexturedRectGeometry(node->geometry(),
                                                targetRect(QSizeF(m_yuv.width, m_yuv.height)),
                                                texRect);
        node->markDirty(QSGNode::DirtyGeometry);
        return node;
    }

    auto *node = static_cast<QSGImageNode *>(oldNode);
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);           // el nodo libera la textura al reemplazarla
        node->setFiltering(QSGTexture::Linear);
    }

    if (m_frameDirty || !node->texture()) {
        QSGTexture *tex = window()->createTextureFromImage(
            m_frame, QQuickWindow::TextureHasAlphaChannel);
        node->setTexture(tex);                // libera la textura anterior (ownsTexture)
        m_frameDirty = false;
        if (qEnvironmentVariableIsSet("PVS_RHI_DEBUG")) {
            static int n = 0;
            ++n;
            if (n <= 3 || n % 30 == 0)
                qInfo("[RHI] textura GPU #%d subida (%dx%d)", n, m_frame.width(), m_frame.height());
        }
    }

    node->setRect(targetRect(QSizeF(m_frame.size())));
    return node;
}
