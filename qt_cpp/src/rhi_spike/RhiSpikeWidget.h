#pragma once

#include <memory>

#include "rhi_compat/RhiWidgetBase.h"

#include "Camera.h"
#include "sim/SimSnapshot.h"

struct NetworkGeometry;
class QRhiCommandBuffer;
class SimWorker;
class VehicleLayerRhi;

// Standalone QRhi spike: renders only vehicles using VehicleLayerRhi on a
// QRhiWidget. Lets us validate the QRhi-based vehicle renderer end-to-end
// independently of the OpenGL NetworkView.
class RhiSpikeWidget : public rhi_compat::RhiWidgetBase {
    Q_OBJECT
public:
    explicit RhiSpikeWidget(QWidget* parent = nullptr);
    ~RhiSpikeWidget() override;

    // Connect this widget to a worker. The worker must outlive the widget.
    void attachWorker(SimWorker* worker);

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private slots:
    void onSnapshot(SimSnapshotPtr snap);
    void onNetworkReady(std::shared_ptr<NetworkGeometry> ng);

private:
    std::unique_ptr<VehicleLayerRhi> m_vehLayer;
    Camera                           m_cam;
    SimSnapshotPtr                   m_pendingSnap;
    bool                             m_haveSnap = false;
    bool                             m_haveNetwork = false;

    // Cache fit bounds; apply once render() has set a valid viewport size.
    bool   m_pendingFit = false;
    float  m_fitMinX = 0, m_fitMinY = 0, m_fitMaxX = 0, m_fitMaxY = 0;

    QRhi* m_lastRhi = nullptr;

    bool   m_dragging = false;
    int    m_lastX    = 0;
    int    m_lastY    = 0;
};
