#pragma once

#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE
class QRhi;
class QRhiCommandBuffer;
class QRhiRenderTarget;
QT_END_NAMESPACE

namespace rhi_compat {

class RhiHostWidgetPrivate;

// Stand-in for QRhiWidget on Qt 6.4 — 6.6 (Qt 6.7 added the real one). The
// protected initialize() / render() / releaseResources() virtuals mirror
// QRhiWidget so the same subclass body compiles against either base.
//
// Implementation: renders into an offscreen QRhiTexture and exposes it via
// the QWidget compositor (same machinery QRhiWidget uses in Qt 6.7+ and
// QOpenGLWidget uses for FBO compositing). No createWindowContainer, no
// per-widget QWindow surface, no QRhiSwapChain — all of which were broken
// on X11/GLX in our earlier shim.
class RhiHostWidget : public QWidget {
    Q_OBJECT
    Q_DECLARE_PRIVATE(RhiHostWidget)
public:
    enum class Api { OpenGL };  // matches what QRhiWidget exposes

    explicit RhiHostWidget(QWidget* parent = nullptr);
    ~RhiHostWidget() override;

    void setApi(Api a) { m_api = a; }
    Api  api() const { return m_api; }

    QRhi*             rhi()          const;
    QRhiRenderTarget* renderTarget() const;

    // QWidget::update() is enough — paintEvent drives rendering.

protected:
    virtual void initialize(QRhiCommandBuffer* cb) { (void)cb; }
    virtual void render(QRhiCommandBuffer* cb)     { (void)cb; }
    virtual void releaseResources()                {}

    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool event(QEvent* e) override;

private:
    Api m_api = Api::OpenGL;
};

}  // namespace rhi_compat
