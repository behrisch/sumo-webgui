#pragma once

#include <QtCore/QPoint>
#include <QtCore/QString>
#include <memory>
#include <vector>

#include "Camera.h"
#include "layers/VehicleLayerRhi.h"
#include "rhi_compat/RhiWidgetBase.h"
#include "rhi_compat/StaticTrisRhi.h"
#include "rhi_compat/TrisPasses.h"
#include "sim/SimSnapshot.h"

struct NetworkGeometry;

class NetworkOverlayWidget;
class PersonLayerRhi;
class POILayerRhi;
class TLSLayerRhi;

class NetworkView : public rhi_compat::RhiWidgetBase {
    Q_OBJECT
    friend class NetworkOverlayWidget;

public:
    explicit NetworkView(QWidget* parent = nullptr);
    ~NetworkView() override;

public slots:
    void setNetwork(std::shared_ptr<NetworkGeometry> ng);
    void setSnapshot(SimSnapshotPtr snap);
    void resetView();
    void setFollowSelected();
    void clearFollow();
    void setVehicleShape(int shape);  // 0=Rect, 1=Triangle, 2=Car, 3=Circle

signals:
    void fpsUpdated(double fps);

protected:
    // RhiWidgetBase / QRhiWidget interface.
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;
    void releaseResources() override;

    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent*  e) override;
    void mouseMoveEvent (QMouseEvent*  e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent     (QWheelEvent*  e) override;

private:
    void uploadStaticGeometry();
    void uploadEdgeColors();
    void pickAt(double pxX, double pxY);

    enum class PickKind { None, Vehicle, Person, TLS, Polygon, POI, Lane, Junction,
                          StoppingPlace, Detector };
    struct Picked {
        PickKind kind = PickKind::None;
        QString  title;
        std::vector<QString> lines;
        QString  id;
    } m_picked;

    Camera m_cam;
    std::shared_ptr<NetworkGeometry> m_ng;
    SimSnapshotPtr                   m_snap;
    SimSnapshotPtr                   m_pendingSnap;
    bool                             m_snapDirty   = false;
    bool                             m_geomDirty   = false;
    bool                             m_edgeRecolor = false;

    // Shared passes (one per topology).
    TrisColorInterleavedPass m_passTris;     // Triangles
    TrisColorInterleavedPass m_passStrip;    // TriangleStrip

    // Static layers (CPU vertex buffers + GPU upload).
    StaticTrisRhi m_detector;
    StaticTrisRhi m_stoppingPlace;
    StaticTrisRhi m_stopLine;
    StaticTrisRhi m_pedSidewalk;
    StaticTrisRhi m_pedWalk;
    StaticTrisRhi m_railSleepers;
    StaticTrisRhi m_railRails;
    StaticTrisRhi m_polygonFills;
    StaticTrisRhi m_polygonOutlines;
    StaticTrisRhi m_netJunctions;
    StaticTrisRhi m_netLaneStrip;             // TriangleStrip
    StaticTrisRhi m_edgeColorStrip;           // TriangleStrip, recolored each frame

    // Per-lane indexing into m_edgeColorStrip's staging vector.
    std::vector<std::uint32_t> m_laneFirst;
    std::vector<std::uint32_t> m_laneCount;
    bool m_edgeColorVisible = false;

    // Full-class dynamic layers.
    std::unique_ptr<VehicleLayerRhi> m_vehicleLayer;
    std::unique_ptr<PersonLayerRhi>  m_personLayer;
    std::unique_ptr<POILayerRhi>     m_poiLayer;
    std::unique_ptr<TLSLayerRhi>     m_tlsLayer;

    // Shape requested via setVehicleShape before m_vehicleLayer exists.
    // Applied at the top of initialize() so a UI selection made during
    // MainWindow construction (i.e. before the first render pass) actually
    // takes effect.
    VehicleLayerRhi::Shape m_pendingVehicleShape = VehicleLayerRhi::Shape::Rectangle;

    NetworkOverlayWidget* m_overlay = nullptr;

    bool   m_panning   = false;
    bool   m_mouseDown = false;
    bool   m_didDrag   = false;
    QPoint m_lastMouse;
    QPoint m_downPos;

    bool   m_hasBounds = false;
    bool   m_needsInitialFit = false;
    double m_minX = 0, m_minY = 0, m_maxX = 0, m_maxY = 0;

    QString m_followId;

    qint64 m_fpsWindowStartMs = 0;
    int    m_fpsFrames        = 0;
};
