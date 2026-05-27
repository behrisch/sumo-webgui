#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>
#include <memory>
#include <vector>

#include "Camera.h"
#include "sim/SimSnapshot.h"

struct NetworkGeometry;
class NetworkLayer;
class VehicleLayer;
class PersonLayer;
class PedAreaLayer;
class POILayer;
class PolygonLayer;
class TLSLayer;
class StopLineLayer;
class RailLayer;
class EdgeColorLayer;
class StoppingPlaceLayer;
class DetectorLayer;

class NetworkView : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT

public:
    explicit NetworkView(QWidget* parent = nullptr);
    ~NetworkView() override;

public slots:
    void setNetwork(std::shared_ptr<NetworkGeometry> ng);
    void setSnapshot(SimSnapshotPtr snap);
    void resetView();
    void setFollowSelected();   // start following the currently picked vehicle
    void clearFollow();         // stop following

signals:
    void fpsUpdated(double fps);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void mousePressEvent(QMouseEvent*  e) override;
    void mouseMoveEvent (QMouseEvent*  e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent     (QWheelEvent*  e) override;

    // QOpenGLWidget supports QPainter overlays via paintEvent override.
    void paintEvent(QPaintEvent* e) override;

private:
    void drawOverlays(QPainter& p);
    void drawScaleBar(QPainter& p);
    void drawLegend  (QPainter& p);
    void drawInfoBox (QPainter& p);
    void pickAt(double pxX, double pxY);

    enum class PickKind { None, Vehicle, Person, TLS, Polygon, POI, Lane, Junction,
                          StoppingPlace, Detector };
    struct Picked {
        PickKind kind = PickKind::None;
        QString  title;
        std::vector<QString> lines;
        QString  id;     // raw id (used for follow mode)
    } m_picked;

    Camera m_cam;
    std::shared_ptr<NetworkGeometry> m_ng;
    SimSnapshotPtr                   m_snap;
    std::unique_ptr<NetworkLayer>    m_networkLayer;
    std::unique_ptr<EdgeColorLayer>  m_edgeColorLayer;
    std::unique_ptr<RailLayer>       m_railLayer;
    std::unique_ptr<StopLineLayer>   m_stopLineLayer;
    std::unique_ptr<StoppingPlaceLayer> m_stoppingPlaceLayer;
    std::unique_ptr<DetectorLayer>   m_detectorLayer;
    std::unique_ptr<PolygonLayer>    m_polygonLayer;
    std::unique_ptr<PedAreaLayer>    m_pedAreaLayer;
    std::unique_ptr<POILayer>        m_poiLayer;
    std::unique_ptr<TLSLayer>        m_tlsLayer;
    std::unique_ptr<VehicleLayer>    m_vehicleLayer;
    std::unique_ptr<PersonLayer>     m_personLayer;
    SimSnapshotPtr                   m_pendingSnap;
    bool                             m_snapDirty = false;

    bool   m_panning   = false;
    bool   m_mouseDown = false;
    bool   m_didDrag   = false;
    QPoint m_lastMouse;
    QPoint m_downPos;

    bool   m_hasBounds = false;
    double m_minX = 0, m_minY = 0, m_maxX = 0, m_maxY = 0;

    // Follow-vehicle mode: when set, every new snapshot recenters camera on
    // this vehicle id. Cleared by clearFollow() or when the vehicle leaves.
    QString m_followId;

    // FPS counter — simple moving average over a 500 ms window.
    qint64 m_fpsWindowStartMs = 0;
    int    m_fpsFrames        = 0;
};
