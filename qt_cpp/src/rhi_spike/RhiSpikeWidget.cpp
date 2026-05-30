#include "RhiSpikeWidget.h"

#include <algorithm>
#include <limits>

#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtGui/QResizeEvent>
#include "rhi_compat/rhi_compat.h"

#include "layers/VehicleLayerRhi.h"
#include "sim/NetworkGeometry.h"
#include "sim/SimWorker.h"

RhiSpikeWidget::RhiSpikeWidget(QWidget* parent)
    : rhi_compat::RhiWidgetBase(parent) {
    // OpenGL backend is most portable on a developer box (no Vulkan SDK
    // assumed). Set QRHI_BACKEND env to override (see QRhi docs).
    rhi_compat::selectOpenGL(this);
    setFocusPolicy(Qt::StrongFocus);
}

RhiSpikeWidget::~RhiSpikeWidget() = default;

void RhiSpikeWidget::attachWorker(SimWorker* worker) {
    connect(worker, &SimWorker::snapshotReady,
            this,   &RhiSpikeWidget::onSnapshot);
    connect(worker, &SimWorker::networkReady,
            this,   &RhiSpikeWidget::onNetworkReady);
}

void RhiSpikeWidget::onSnapshot(SimSnapshotPtr snap) {
    m_pendingSnap = std::move(snap);
    m_haveSnap = true;
    update();
}

void RhiSpikeWidget::onNetworkReady(std::shared_ptr<NetworkGeometry> ng) {
    if (!ng || ng->lane_points.empty()) return;
    float minX =  std::numeric_limits<float>::infinity();
    float minY =  std::numeric_limits<float>::infinity();
    float maxX = -std::numeric_limits<float>::infinity();
    float maxY = -std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i + 1 < ng->lane_points.size(); i += 2) {
        minX = std::min(minX, ng->lane_points[i]);
        maxX = std::max(maxX, ng->lane_points[i]);
        minY = std::min(minY, ng->lane_points[i + 1]);
        maxY = std::max(maxY, ng->lane_points[i + 1]);
    }
    // Defer the actual fit until render() has set the real viewport size;
    // fitBounds depends on viewport pixels (which are 1x1 at this point).
    m_fitMinX = minX; m_fitMinY = minY;
    m_fitMaxX = maxX; m_fitMaxY = maxY;
    m_pendingFit = true;
    m_haveNetwork = true;
    update();
}

void RhiSpikeWidget::initialize(QRhiCommandBuffer* /*cb*/) {
    QRhi* r = rhi();
    if (!r) return;

    if (!m_vehLayer) m_vehLayer = std::make_unique<VehicleLayerRhi>();

    if (m_lastRhi != r) {
        // RHI was rebuilt (e.g. swapchain resize / reparent). Recreate
        // pipeline objects against the new device and render pass.
        m_vehLayer->release();
        m_lastRhi = r;
    }

    QRhiRenderPassDescriptor* rp = renderTarget()->renderPassDescriptor();
    m_vehLayer->initialize(r, rp, renderTarget()->sampleCount());
}

void RhiSpikeWidget::render(QRhiCommandBuffer* cb) {
    QRhi* r = rhi();
    if (!r || !m_vehLayer) return;

    // Resize camera to the colour attachment size (already in device pixels).
    const QSize px = renderTarget()->pixelSize();
    m_cam.setViewport(px.width(), px.height());

    // Now that the viewport is correct, apply any pending fit-bounds request.
    if (m_pendingFit && px.width() > 1 && px.height() > 1) {
        m_cam.fitBounds(m_fitMinX, m_fitMinY, m_fitMaxX, m_fitMaxY);
        m_pendingFit = false;
    }

    if (m_pendingSnap) {
        m_vehLayer->setSnapshot(m_pendingSnap);
        m_pendingSnap.reset();
    }

    const auto proj = m_cam.projection();
    m_vehLayer->setProjection(proj.data());

    QRhiResourceUpdateBatch* batch = r->nextResourceUpdateBatch();
    m_vehLayer->resourceUpdate(batch);

    const QColor clear = m_haveNetwork ? QColor(20, 20, 20) : QColor(40, 0, 0);
    cb->beginPass(renderTarget(),
                  QColor(clear),
                  { 1.0f, 0 },
                  batch);
    m_vehLayer->render(cb);
    cb->endPass();
}

void RhiSpikeWidget::releaseResources() {
    if (m_vehLayer) m_vehLayer->release();
    m_lastRhi = nullptr;
}

void RhiSpikeWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_lastX = e->position().x();
        m_lastY = e->position().y();
    }
}

void RhiSpikeWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!m_dragging) return;
    const int dx = e->position().x() - m_lastX;
    const int dy = e->position().y() - m_lastY;
    m_lastX = e->position().x();
    m_lastY = e->position().y();
    m_cam.panPixels(dx, dy);
    update();
}

void RhiSpikeWidget::wheelEvent(QWheelEvent* e) {
    const int dy = e->angleDelta().y();
    if (dy == 0) return;
    const double f = (dy > 0) ? 1.2 : 1.0 / 1.2;
    m_cam.zoomAtPixel(e->position().x(), e->position().y(), f);
    update();
}

void RhiSpikeWidget::resizeEvent(QResizeEvent* e) {
    rhi_compat::RhiWidgetBase::resizeEvent(e);
    m_cam.setViewport(e->size().width(), e->size().height());
}
