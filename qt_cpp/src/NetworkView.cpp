#include "NetworkView.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QSurfaceFormat>
#include <QWheelEvent>
#include <cmath>

#include "layers/NetworkLayer.h"
#include "layers/VehicleLayer.h"
#include "sim/NetworkGeometry.h"

NetworkView::NetworkView(QWidget* parent) : QOpenGLWidget(parent) {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setSamples(4);
    setFormat(fmt);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

NetworkView::~NetworkView() {
    makeCurrent();
    m_networkLayer.reset();
    m_vehicleLayer.reset();
    doneCurrent();
}

void NetworkView::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);

    m_networkLayer = std::make_unique<NetworkLayer>();
    m_networkLayer->initGL(this);
    if (m_ng) m_networkLayer->setGeometry(m_ng);

    m_vehicleLayer = std::make_unique<VehicleLayer>();
    m_vehicleLayer->initGL(this);
    if (m_pendingSnap) {
        m_vehicleLayer->setSnapshot(m_pendingSnap);
        m_snapDirty = false;
    }
}

void NetworkView::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
    m_cam.setViewport(w, h);
    if (m_hasBounds && m_cam.pixelsPerUnit() <= 1.0) {
        // First useful resize: frame the network.
        m_cam.fitBounds(m_minX, m_minY, m_maxX, m_maxY);
    }
}

void NetworkView::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);
    const auto proj = m_cam.projection();
    if (m_networkLayer) m_networkLayer->draw(proj.data());
    if (m_vehicleLayer) {
        if (m_snapDirty) {
            m_vehicleLayer->setSnapshot(m_pendingSnap);
            m_snapDirty = false;
        }
        m_vehicleLayer->draw(proj.data());
    }

    // FPS sampling.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_fpsWindowStartMs == 0) m_fpsWindowStartMs = now;
    ++m_fpsFrames;
    const qint64 dt = now - m_fpsWindowStartMs;
    if (dt >= 500) {
        const double fps = 1000.0 * m_fpsFrames / static_cast<double>(dt);
        emit fpsUpdated(fps);
        m_fpsFrames = 0;
        m_fpsWindowStartMs = now;
    }
}

void NetworkView::setSnapshot(SimSnapshotPtr snap) {
    m_pendingSnap = std::move(snap);
    m_snapDirty = true;
    update();
}

void NetworkView::paintEvent(QPaintEvent* e) {
    QOpenGLWidget::paintEvent(e);  // runs paintGL
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    drawOverlays(p);
}

void NetworkView::setNetwork(std::shared_ptr<NetworkGeometry> ng) {
    m_ng = std::move(ng);
    if (m_ng) {
        m_hasBounds = true;
        m_minX = m_ng->min_x; m_minY = m_ng->min_y;
        m_maxX = m_ng->max_x; m_maxY = m_ng->max_y;
        m_cam.setViewport(width(), height());
        m_cam.fitBounds(m_minX, m_minY, m_maxX, m_maxY);
    }
    if (m_networkLayer) {
        makeCurrent();
        m_networkLayer->setGeometry(m_ng);
        doneCurrent();
    }
    update();
}

void NetworkView::resetView() {
    if (!m_hasBounds) return;
    m_cam.fitBounds(m_minX, m_minY, m_maxX, m_maxY);
    update();
}

void NetworkView::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_panning = true;
        m_lastMouse = e->pos();
    }
}

void NetworkView::mouseMoveEvent(QMouseEvent* e) {
    if (m_panning) {
        const QPoint d = e->pos() - m_lastMouse;
        m_lastMouse = e->pos();
        m_cam.panPixels(d.x(), d.y());
        update();
    }
}

void NetworkView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) m_panning = false;
}

void NetworkView::wheelEvent(QWheelEvent* e) {
    const double steps  = e->angleDelta().y() / 120.0;
    const double factor = std::pow(1.2, steps);
    const QPointF pos = e->position();
    m_cam.zoomAtPixel(pos.x(), pos.y(), factor);
    update();
    e->accept();
}

void NetworkView::drawOverlays(QPainter& p) {
    drawScaleBar(p);
}

void NetworkView::drawScaleBar(QPainter& p) {
    // Pick a "nice" meter length that's 60..120 px wide.
    const double ppu = m_cam.pixelsPerUnit();
    if (!(ppu > 0.0)) return;
    const double targetPx = 100.0;
    double meters = targetPx / ppu;
    // Round to 1, 2, 5 * 10^k.
    const double mag = std::pow(10.0, std::floor(std::log10(meters)));
    const double mantissa = meters / mag;
    double nice;
    if      (mantissa < 1.5) nice = 1.0;
    else if (mantissa < 3.5) nice = 2.0;
    else if (mantissa < 7.5) nice = 5.0;
    else                     nice = 10.0;
    meters = nice * mag;
    const double widthPx = meters * ppu;

    // Label.
    QString label;
    if (meters >= 1000.0) label = QString::number(meters / 1000.0, 'g', 3) + " km";
    else if (meters >= 1.0) label = QString::number(meters, 'g', 3) + " m";
    else label = QString::number(meters * 100.0, 'g', 3) + " cm";

    // Anchor bottom-right with 12 px margin.
    const int margin = 12;
    const QFont f = p.font();
    const QFontMetrics fm(f);
    const int barH = 6;
    const int textH = fm.height();
    const int boxW = static_cast<int>(widthPx) + 2 * margin;
    const int boxH = barH + textH + 8;
    const QRectF box(width() - boxW - margin, height() - boxH - margin,
                     boxW, boxH);
    p.fillRect(box, QColor(0, 0, 0, 160));
    p.setPen(Qt::white);

    const int x0 = static_cast<int>(box.left()) + margin;
    const int y0 = static_cast<int>(box.bottom()) - 4 - barH;
    p.drawLine(x0, y0, x0 + static_cast<int>(widthPx), y0);
    p.drawLine(x0, y0 - 3, x0, y0 + 3);
    p.drawLine(x0 + static_cast<int>(widthPx), y0 - 3,
               x0 + static_cast<int>(widthPx), y0 + 3);
    p.drawText(static_cast<int>(box.left()) + margin,
               static_cast<int>(box.top()) + fm.ascent() + 2,
               label);
}
