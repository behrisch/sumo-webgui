#include "rhi_compat/RhiHostWidget.h"

#include <QtCore/qglobal.h>

#if QT_VERSION < QT_VERSION_CHECK(6, 7, 0) || defined(RHI_COMPAT_FORCE_FALLBACK)

#include "rhi_compat/rhi_compat.h"

#include <QtCore/QEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QWindow>
#include <QtGui/qpa/qplatformbackingstore.h>
#include <QtGui/private/qbackingstorerhisupport_p.h>

// QWidgetPrivate is a "true" private — pull it via the standard private
// include path that comes with qt6-base-private-dev.
#include <private/qwidget_p.h>
#include <private/qwidgetrepaintmanager_p.h>

namespace rhi_compat {

// --- QWidgetPrivate subclass --------------------------------------------------
//
// Mirrors QRhiWidgetPrivate from Qt 6.8 but cut down to what we actually need.
// Lives in the .cpp on purpose — nothing outside this translation unit needs
// to know about Qt's widget-private internals.
class RhiHostWidgetPrivate : public QWidgetPrivate {
    Q_DECLARE_PUBLIC(RhiHostWidget)
public:
    // ---- virtuals consumed by QWidgetRepaintManager/QBackingStore ----------
    QRhiTexture* texture() const override {
        return textureInvalid ? nullptr : colorTexture;
    }
    QPlatformTextureList::Flags textureListFlags() override {
        return QWidgetPrivate::textureListFlags();
    }
    QPlatformBackingStoreRhiConfig rhiConfig() const override { return config; }
    void endCompose() override { /* nothing to do; matches QRhiWidget */ }

    // ---- our own helpers ---------------------------------------------------
    void ensureRhi();
    void ensureTexture(bool* changed);
    bool invokeInitialize(QRhiCommandBuffer* cb);
    void resetRenderTargetObjects();
    void resetColorBufferObjects();
    void releaseAllRhiResources();

    QPlatformBackingStoreRhiConfig config;
    QRhi*                          rhi = nullptr;
    QBackingStoreRhiSupport        offscreenRenderer;

    QRhiTexture*              colorTexture = nullptr;
    QRhiRenderBuffer*         depthStencilBuffer = nullptr;
    QRhiTextureRenderTarget*  renderTarget = nullptr;
    QRhiRenderPassDescriptor* rpDesc = nullptr;

    bool noSize         = false;
    bool textureInvalid = false;
    bool initialized    = false;
};

void RhiHostWidgetPrivate::resetColorBufferObjects() {
    if (colorTexture) { colorTexture->deleteLater(); colorTexture = nullptr; }
}
void RhiHostWidgetPrivate::resetRenderTargetObjects() {
    if (renderTarget)        { renderTarget->deleteLater();        renderTarget = nullptr; }
    if (rpDesc)              { rpDesc->deleteLater();              rpDesc = nullptr; }
    if (depthStencilBuffer)  { depthStencilBuffer->deleteLater();  depthStencilBuffer = nullptr; }
}
void RhiHostWidgetPrivate::releaseAllRhiResources() {
    resetRenderTargetObjects();
    resetColorBufferObjects();
    initialized = false;
}

void RhiHostWidgetPrivate::ensureRhi() {
    Q_Q(RhiHostWidget);

    // Try the top-level's backing-store RHI first. Available once the toplevel
    // is exposed — its backing store creates a QRhi using our rhiConfig().
    QRhi* topRhi = nullptr;
    if (auto* rpm = QWidgetPrivate::get(q->window())->maybeRepaintManager())
        topRhi = rpm->rhi();

    if (!topRhi) {
        // Fall back to an offscreen RHI so the widget can still render
        // (e.g. for grabFramebuffer or pre-exposure paints). The texture is
        // valid but won't appear on screen until the toplevel RHI is up.
        if (!offscreenRenderer.rhi()) {
            offscreenRenderer.setConfig(config);
            offscreenRenderer.create();
        }
        topRhi = offscreenRenderer.rhi();
    }

    if (rhi && rhi != topRhi) {
        // Toplevel switched RHIs under us (e.g. window reparented). Drop
        // resources tied to the old RHI.
        q->releaseResources();
        releaseAllRhiResources();
    }
    rhi = topRhi;
}

void RhiHostWidgetPrivate::ensureTexture(bool* changed) {
    Q_Q(RhiHostWidget);
    QSize newSize = q->size() * q->devicePixelRatio();
    const int minSz = rhi->resourceLimit(QRhi::TextureSizeMin);
    const int maxSz = rhi->resourceLimit(QRhi::TextureSizeMax);
    newSize.setWidth (qBound(minSz, newSize.width(),  maxSz));
    newSize.setHeight(qBound(minSz, newSize.height(), maxSz));

    if (!colorTexture) {
        if (changed) *changed = true;
        colorTexture = rhi->newTexture(QRhiTexture::RGBA8, newSize, 1,
                                       QRhiTexture::RenderTarget |
                                       QRhiTexture::UsedAsTransferSource);
        if (!colorTexture->create()) {
            qWarning("RhiHostWidget: failed to create colour texture");
            delete colorTexture; colorTexture = nullptr;
            return;
        }
    } else if (colorTexture->pixelSize() != newSize) {
        if (changed) *changed = true;
        colorTexture->setPixelSize(newSize);
        if (!colorTexture->create())
            qWarning("RhiHostWidget: failed to resize colour texture");
        // resizing the colour texture invalidates the render target — drop it.
        resetRenderTargetObjects();
    }
    textureInvalid = false;
}

bool RhiHostWidgetPrivate::invokeInitialize(QRhiCommandBuffer* cb) {
    Q_Q(RhiHostWidget);
    if (!colorTexture) return false;

    const QSize px = colorTexture->pixelSize();
    if (!depthStencilBuffer) {
        depthStencilBuffer = rhi->newRenderBuffer(
            QRhiRenderBuffer::DepthStencil, px, 1);
        if (!depthStencilBuffer->create()) {
            qWarning("RhiHostWidget: failed to create depth-stencil buffer");
            resetRenderTargetObjects();
            return false;
        }
    } else if (depthStencilBuffer->pixelSize() != px) {
        depthStencilBuffer->setPixelSize(px);
        depthStencilBuffer->create();
    }
    if (!renderTarget) {
        QRhiColorAttachment att(colorTexture);
        QRhiTextureRenderTargetDescription desc(att, depthStencilBuffer);
        renderTarget = rhi->newTextureRenderTarget(desc);
        rpDesc = renderTarget->newCompatibleRenderPassDescriptor();
        renderTarget->setRenderPassDescriptor(rpDesc);
        if (!renderTarget->create()) {
            qWarning("RhiHostWidget: failed to create render target");
            resetRenderTargetObjects();
            return false;
        }
    }

    q->initialize(cb);
    initialized = true;
    return true;
}

// --- RhiHostWidget ------------------------------------------------------------

RhiHostWidget::RhiHostWidget(QWidget* parent)
    : QWidget(*(new RhiHostWidgetPrivate), parent, Qt::WindowFlags()) {
    Q_D(RhiHostWidget);
    d->setRenderToTexture();
    d->config.setEnabled(true);
    d->config.setApi(QPlatformBackingStoreRhiConfig::OpenGL);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

RhiHostWidget::~RhiHostWidget() {
    Q_D(RhiHostWidget);
    if (d->rhi) {
        d->releaseAllRhiResources();
    }
    d->offscreenRenderer.reset();
}

QRhi* RhiHostWidget::rhi() const {
    Q_D(const RhiHostWidget);
    return d->rhi;
}

QRhiRenderTarget* RhiHostWidget::renderTarget() const {
    Q_D(const RhiHostWidget);
    return d->renderTarget;
}

void RhiHostWidget::resizeEvent(QResizeEvent* e) {
    Q_D(RhiHostWidget);
    if (e->size().isEmpty()) { d->noSize = true; return; }
    d->noSize = false;
    QWidget::resizeEvent(e);
    update();
}

void RhiHostWidget::paintEvent(QPaintEvent*) {
    Q_D(RhiHostWidget);
    if (!updatesEnabled() || d->noSize) return;

    d->ensureRhi();
    if (!d->rhi) return;

    QRhiCommandBuffer* cb = nullptr;
    if (d->rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return;

    bool needsInit = false;
    d->ensureTexture(&needsInit);
    if (d->colorTexture) {
        bool ok = true;
        if (needsInit || !d->initialized)
            ok = d->invokeInitialize(cb);
        if (ok) render(cb);
    }
    d->rhi->endOffscreenFrame();
}

bool RhiHostWidget::event(QEvent* e) {
    Q_D(RhiHostWidget);
    switch (e->type()) {
        case QEvent::WindowAboutToChangeInternal:
            // Toplevel is about to change — its RHI may go away. Mark texture
            // invalid so the compositor stops dereferencing the old object.
            d->textureInvalid = true;
            if (d->rhi) {
                releaseResources();      // notify user code
                d->releaseAllRhiResources();
                // keep d->rhi pointer for ensureRhi() comparison; will be
                // replaced on next paint.
            }
            break;
        case QEvent::Show:
            if (isVisible()) update();
            break;
        default: break;
    }
    return QWidget::event(e);
}

}  // namespace rhi_compat

#include "moc_RhiHostWidget.cpp"

#endif  // Qt < 6.7 || forced fallback
