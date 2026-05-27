#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPoint>
#include <memory>

#include "Camera.h"
#include "sim/SimSnapshot.h"

struct NetworkGeometry;
class NetworkLayer;
class VehicleLayer;
class PersonLayer;
class PolygonLayer;
class TLSLayer;

class NetworkView : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT

public:
    explicit NetworkView(QWidget* parent = nullptr);
    ~NetworkView() override;

public slots:
    void setNetwork(std::shared_ptr<NetworkGeometry> ng);
    void setSnapshot(SimSnapshotPtr snap);
    void resetView();

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

    Camera m_cam;
    std::shared_ptr<NetworkGeometry> m_ng;
    std::unique_ptr<NetworkLayer>    m_networkLayer;
    std::unique_ptr<PolygonLayer>    m_polygonLayer;
    std::unique_ptr<TLSLayer>        m_tlsLayer;
    std::unique_ptr<VehicleLayer>    m_vehicleLayer;
    std::unique_ptr<PersonLayer>     m_personLayer;
    SimSnapshotPtr                   m_pendingSnap;
    bool                             m_snapDirty = false;

    bool   m_panning = false;
    QPoint m_lastMouse;

    bool   m_hasBounds = false;
    double m_minX = 0, m_minY = 0, m_maxX = 0, m_maxY = 0;

    // FPS counter — simple moving average over a 500 ms window.
    qint64 m_fpsWindowStartMs = 0;
    int    m_fpsFrames        = 0;
};
