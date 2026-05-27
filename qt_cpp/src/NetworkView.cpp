#include "NetworkView.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QSurfaceFormat>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

#include "layers/DetectorLayer.h"
#include "layers/EdgeColorLayer.h"
#include "layers/NetworkLayer.h"
#include "layers/PersonLayer.h"
#include "layers/PolygonLayer.h"
#include "layers/RailLayer.h"
#include "layers/StopLineLayer.h"
#include "layers/StoppingPlaceLayer.h"
#include "layers/TLSLayer.h"
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
    m_edgeColorLayer.reset();
    m_railLayer.reset();
    m_stopLineLayer.reset();
    m_stoppingPlaceLayer.reset();
    m_detectorLayer.reset();
    m_polygonLayer.reset();
    m_tlsLayer.reset();
    m_vehicleLayer.reset();
    m_personLayer.reset();
    doneCurrent();
}

void NetworkView::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_networkLayer   = std::make_unique<NetworkLayer>();   m_networkLayer  ->initGL(this);
    m_edgeColorLayer = std::make_unique<EdgeColorLayer>(); m_edgeColorLayer->initGL(this);
    m_railLayer      = std::make_unique<RailLayer>();      m_railLayer     ->initGL(this);
    m_stopLineLayer  = std::make_unique<StopLineLayer>();  m_stopLineLayer ->initGL(this);
    m_stoppingPlaceLayer = std::make_unique<StoppingPlaceLayer>();
    m_stoppingPlaceLayer->initGL(this);
    m_detectorLayer  = std::make_unique<DetectorLayer>();  m_detectorLayer ->initGL(this);
    m_polygonLayer   = std::make_unique<PolygonLayer>();   m_polygonLayer  ->initGL(this);
    m_tlsLayer       = std::make_unique<TLSLayer>();       m_tlsLayer      ->initGL(this);
    m_vehicleLayer   = std::make_unique<VehicleLayer>();   m_vehicleLayer  ->initGL(this);
    m_personLayer    = std::make_unique<PersonLayer>();    m_personLayer   ->initGL(this);

    if (m_ng) {
        m_networkLayer  ->setGeometry(m_ng);
        m_edgeColorLayer->setGeometry(m_ng);
        m_railLayer     ->setGeometry(m_ng);
        m_stopLineLayer ->setGeometry(m_ng);
        m_stoppingPlaceLayer->setGeometry(m_ng);
        m_detectorLayer ->setGeometry(m_ng);
        m_polygonLayer  ->setGeometry(m_ng);
        m_tlsLayer      ->setGeometry(m_ng);
    }
    if (m_pendingSnap) {
        m_vehicleLayer->setSnapshot(m_pendingSnap);
        m_personLayer ->setSnapshot(m_pendingSnap);
        m_tlsLayer    ->setSnapshot(m_pendingSnap);
        m_edgeColorLayer->setActive(!m_pendingSnap->lane_attr_rgba.empty());
        if (!m_pendingSnap->lane_attr_rgba.empty())
            m_edgeColorLayer->setLaneColors(m_pendingSnap->lane_attr_rgba);
        m_snap = m_pendingSnap;
        m_snapDirty = false;
    }
}

void NetworkView::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
    m_cam.setViewport(w, h);
    if (m_hasBounds && m_cam.pixelsPerUnit() <= 1.0) {
        m_cam.fitBounds(m_minX, m_minY, m_maxX, m_maxY);
    }
}

void NetworkView::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);
    const auto proj = m_cam.projection();

    if (m_snapDirty) {
        if (m_vehicleLayer) m_vehicleLayer->setSnapshot(m_pendingSnap);
        if (m_personLayer)  m_personLayer ->setSnapshot(m_pendingSnap);
        if (m_tlsLayer)     m_tlsLayer    ->setSnapshot(m_pendingSnap);
        if (m_edgeColorLayer) {
            const bool hasColors = m_pendingSnap
                && !m_pendingSnap->lane_attr_rgba.empty();
            m_edgeColorLayer->setActive(hasColors);
            if (hasColors) m_edgeColorLayer->setLaneColors(m_pendingSnap->lane_attr_rgba);
        }
        m_snap = m_pendingSnap;
        m_snapDirty = false;
    }

    // Draw order: junctions+roads -> per-lane color overlay -> rails ->
    // polygons -> stop lines -> TLS heads -> vehicles -> persons.
    if (m_networkLayer)   m_networkLayer  ->draw(proj.data());
    if (m_edgeColorLayer) m_edgeColorLayer->draw(proj.data());
    if (m_railLayer)      m_railLayer     ->draw(proj.data());
    if (m_polygonLayer)   m_polygonLayer  ->draw(proj.data());
    if (m_stoppingPlaceLayer) m_stoppingPlaceLayer->draw(proj.data());
    if (m_detectorLayer)  m_detectorLayer ->draw(proj.data());
    if (m_stopLineLayer)  m_stopLineLayer ->draw(proj.data());
    if (m_tlsLayer)       m_tlsLayer      ->draw(proj.data());
    if (m_vehicleLayer)   m_vehicleLayer  ->draw(proj.data());
    if (m_personLayer)    m_personLayer   ->draw(proj.data());

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
    m_picked = Picked{};  // pickings against old geometry no longer valid
    if (m_ng) {
        m_hasBounds = true;
        m_minX = m_ng->min_x; m_minY = m_ng->min_y;
        m_maxX = m_ng->max_x; m_maxY = m_ng->max_y;
        m_cam.setViewport(width(), height());
        m_cam.fitBounds(m_minX, m_minY, m_maxX, m_maxY);
    }
    if (m_networkLayer) {
        makeCurrent();
        m_networkLayer  ->setGeometry(m_ng);
        if (m_edgeColorLayer) m_edgeColorLayer->setGeometry(m_ng);
        if (m_railLayer)      m_railLayer     ->setGeometry(m_ng);
        if (m_stopLineLayer)  m_stopLineLayer ->setGeometry(m_ng);
        if (m_stoppingPlaceLayer) m_stoppingPlaceLayer->setGeometry(m_ng);
        if (m_detectorLayer)  m_detectorLayer ->setGeometry(m_ng);
        if (m_polygonLayer)   m_polygonLayer  ->setGeometry(m_ng);
        if (m_tlsLayer)       m_tlsLayer      ->setGeometry(m_ng);
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
        m_mouseDown = true;
        m_didDrag = false;
        m_downPos = e->pos();
        m_lastMouse = e->pos();
    }
}

void NetworkView::mouseMoveEvent(QMouseEvent* e) {
    if (m_mouseDown) {
        const QPoint d = e->pos() - m_lastMouse;
        m_lastMouse = e->pos();
        if (!m_didDrag) {
            if ((e->pos() - m_downPos).manhattanLength() > 4) {
                m_didDrag = true;
                m_panning = true;
            }
        }
        if (m_panning) {
            m_cam.panPixels(d.x(), d.y());
            update();
        }
    }
}

void NetworkView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const bool wasClick = m_mouseDown && !m_didDrag;
    m_mouseDown = false;
    m_panning = false;
    if (wasClick) {
        pickAt(e->position().x(), e->position().y());
        update();
    }
}

void NetworkView::wheelEvent(QWheelEvent* e) {
    const double steps  = e->angleDelta().y() / 120.0;
    const double factor = std::pow(1.2, steps);
    const QPointF pos = e->position();
    m_cam.zoomAtPixel(pos.x(), pos.y(), factor);
    update();
    e->accept();
}

// ---------------------------- picking ---------------------------------

namespace {
bool pointInPoly(float x, float y, const float* pts, std::size_t n) {
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const float xi = pts[i * 2], yi = pts[i * 2 + 1];
        const float xj = pts[j * 2], yj = pts[j * 2 + 1];
        const bool crosses = ((yi > y) != (yj > y))
            && (x < (xj - xi) * (y - yi) / ((yj - yi) + 1e-12f) + xi);
        if (crosses) inside = !inside;
    }
    return inside;
}

// Distance squared from point to segment AB.
float distSqToSeg(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    const float cx = ax + t * dx, cy = ay + t * dy;
    const float ex = px - cx, ey = py - cy;
    return ex * ex + ey * ey;
}

const char* laneKindName(std::uint8_t k) {
    switch (k) {
        case 0: return "road";
        case 1: return "rail";
        case 2: return "sidewalk";
        case 3: return "walkingarea/crossing";
        case 4: return "internal";
    }
    return "other";
}
}  // namespace

void NetworkView::pickAt(double pxX, double pxY) {
    if (!m_ng) return;
    double wx, wy;
    m_cam.pixelToWorld(pxX, pxY, wx, wy);
    const float fx = static_cast<float>(wx);
    const float fy = static_cast<float>(wy);
    const double ppu = std::max(1e-6, m_cam.pixelsPerUnit());
    const float pickRadius = static_cast<float>(8.0 / ppu);  // 8 px tolerance
    const float pickR2 = pickRadius * pickRadius;

    Picked best;
    best.kind = PickKind::None;

    // 1) Vehicles (closest within ~vehicle radius or 8px).
    if (m_snap) {
        float bestD2 = std::numeric_limits<float>::infinity();
        std::size_t bestIdx = 0;
        for (std::size_t i = 0; i < m_snap->vehicle_count(); ++i) {
            const float dx = m_snap->x[i] - fx;
            const float dy = m_snap->y[i] - fy;
            const float d2 = dx * dx + dy * dy;
            const float tol2 = std::max(pickR2, 4.0f);  // ~2m car radius
            if (d2 < tol2 && d2 < bestD2) { bestD2 = d2; bestIdx = i; }
        }
        if (bestD2 < std::numeric_limits<float>::infinity()) {
            best.kind = PickKind::Vehicle;
            best.title = QString("Vehicle  %1").arg(
                QString::fromStdString(m_snap->ids[bestIdx]));
            best.lines.push_back(QString("position  %1, %2 m")
                .arg(m_snap->x[bestIdx], 0, 'f', 1)
                .arg(m_snap->y[bestIdx], 0, 'f', 1));
            const double ang = std::atan2(m_snap->sin_a[bestIdx],
                                          m_snap->cos_a[bestIdx]) * 180.0 / M_PI;
            best.lines.push_back(QString("heading   %1°").arg(ang, 0, 'f', 1));
        }

        if (best.kind == PickKind::None) {
            bestD2 = std::numeric_limits<float>::infinity();
            for (std::size_t i = 0; i < m_snap->person_count(); ++i) {
                const float dx = m_snap->person_x[i] - fx;
                const float dy = m_snap->person_y[i] - fy;
                const float d2 = dx * dx + dy * dy;
                if (d2 < pickR2 && d2 < bestD2) { bestD2 = d2; bestIdx = i; }
            }
            if (bestD2 < std::numeric_limits<float>::infinity()) {
                best.kind = PickKind::Person;
                best.title = QString("Person  %1").arg(
                    QString::fromStdString(m_snap->person_ids[bestIdx]));
                best.lines.push_back(QString("position  %1, %2 m")
                    .arg(m_snap->person_x[bestIdx], 0, 'f', 1)
                    .arg(m_snap->person_y[bestIdx], 0, 'f', 1));
            }
        }
    }

    // 2) TLS heads.
    if (best.kind == PickKind::None) {
        const std::size_t n = m_ng->tls_marker_count();
        float bestD2 = std::numeric_limits<float>::infinity();
        std::size_t bestIdx = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const float dx = m_ng->tls_x[i] - fx;
            const float dy = m_ng->tls_y[i] - fy;
            const float d2 = dx * dx + dy * dy;
            const float tol2 = std::max(pickR2, 1.44f);  // 1.2m head radius
            if (d2 < tol2 && d2 < bestD2) { bestD2 = d2; bestIdx = i; }
        }
        if (bestD2 < std::numeric_limits<float>::infinity()) {
            best.kind = PickKind::TLS;
            const QString id = QString::fromStdString(m_ng->tls_ids[bestIdx]);
            best.title = QString("Traffic light  %1").arg(id);
            best.lines.push_back(QString("link index  %1").arg(m_ng->tls_state_index[bestIdx]));
            if (m_snap) {
                auto it = m_snap->tls_states.find(m_ng->tls_ids[bestIdx]);
                if (it != m_snap->tls_states.end()) {
                    const auto& st = it->second;
                    const std::uint32_t li = m_ng->tls_state_index[bestIdx];
                    best.lines.push_back(QString("state       %1").arg(
                        QString::fromStdString(st)));
                    if (li < st.size()) {
                        const char c = st[li];
                        best.lines.push_back(QString("this link   %1").arg(QChar(c)));
                    }
                }
            }
        }
    }

    // 3) Polygons.
    if (best.kind == PickKind::None) {
        for (std::size_t i = 0; i < m_ng->polygon_count(); ++i) {
            const std::uint32_t s = m_ng->polygon_offsets[i];
            const std::uint32_t e = m_ng->polygon_offsets[i + 1];
            const std::size_t n = e - s;
            if (n < 3) continue;
            if (pointInPoly(fx, fy, &m_ng->polygon_points[s * 2], n)) {
                best.kind = PickKind::Polygon;
                best.title = QString("Polygon  #%1").arg(i);
                best.lines.push_back(QString("filled    %1")
                    .arg(m_ng->polygon_filled[i] ? "yes" : "no"));
                best.lines.push_back(QString("color     rgba(%1,%2,%3,%4)")
                    .arg(m_ng->polygon_rgba[i * 4])
                    .arg(m_ng->polygon_rgba[i * 4 + 1])
                    .arg(m_ng->polygon_rgba[i * 4 + 2])
                    .arg(m_ng->polygon_rgba[i * 4 + 3]));
                break;
            }
        }
    }

    // 3b) Stopping places: closest within half of band length.
    if (best.kind == PickKind::None) {
        float bestD2 = std::numeric_limits<float>::infinity();
        std::size_t bestIdx = 0;
        for (std::size_t i = 0; i < m_ng->stop_count(); ++i) {
            const float dx = m_ng->stop_x[i] - fx;
            const float dy = m_ng->stop_y[i] - fy;
            const float d2 = dx * dx + dy * dy;
            const float tol = std::max(m_ng->stop_len[i],
                                       m_ng->stop_w[i]) * 0.5f + 1.0f;
            const float tol2 = std::max(pickR2, tol * tol);
            if (d2 < tol2 && d2 < bestD2) { bestD2 = d2; bestIdx = i; }
        }
        if (bestD2 < std::numeric_limits<float>::infinity()) {
            best.kind = PickKind::StoppingPlace;
            const char* k = "stop";
            switch (m_ng->stop_kind[bestIdx]) {
                case 0: k = "Bus stop"; break;
                case 1: k = "Charging station"; break;
                case 2: k = "Parking area"; break;
            }
            best.title = QString("%1  %2").arg(k,
                QString::fromStdString(m_ng->stop_ids[bestIdx]));
            best.lines.push_back(QString("length    %1 m")
                .arg(m_ng->stop_len[bestIdx], 0, 'f', 1));
        }
    }

    // 3c) Detectors.
    if (best.kind == PickKind::None) {
        float bestD2 = std::numeric_limits<float>::infinity();
        std::size_t bestIdx = 0;
        for (std::size_t i = 0; i < m_ng->det_count(); ++i) {
            const float dx = m_ng->det_x[i] - fx;
            const float dy = m_ng->det_y[i] - fy;
            const float d2 = dx * dx + dy * dy;
            const float tol = std::max(m_ng->det_len[i], 2.0f) * 0.5f + 1.0f;
            const float tol2 = std::max(pickR2, tol * tol);
            if (d2 < tol2 && d2 < bestD2) { bestD2 = d2; bestIdx = i; }
        }
        if (bestD2 < std::numeric_limits<float>::infinity()) {
            best.kind = PickKind::Detector;
            const char* k = m_ng->det_kind[bestIdx] == 0
                ? "Induction loop" : "Lane-area detector";
            best.title = QString("%1  %2").arg(k,
                QString::fromStdString(m_ng->det_ids[bestIdx]));
            if (m_ng->det_len[bestIdx] > 0)
                best.lines.push_back(QString("length    %1 m")
                    .arg(m_ng->det_len[bestIdx], 0, 'f', 1));
        }
    }

    // 4) Lanes (closest within half-width).
    if (best.kind == PickKind::None) {
        float bestD2 = std::numeric_limits<float>::infinity();
        std::size_t bestIdx = 0;
        for (std::size_t i = 0; i < m_ng->lane_count(); ++i) {
            const std::uint32_t s = m_ng->lane_offsets[i];
            const std::uint32_t e = m_ng->lane_offsets[i + 1];
            if (e - s < 2) continue;
            const float halfW = 0.5f * m_ng->lane_widths[i];
            const float laneTol2 = std::max(pickR2, halfW * halfW);
            for (std::uint32_t k = s; k + 1 < e; ++k) {
                const float d2 = distSqToSeg(fx, fy,
                    m_ng->lane_points[k * 2], m_ng->lane_points[k * 2 + 1],
                    m_ng->lane_points[(k + 1) * 2], m_ng->lane_points[(k + 1) * 2 + 1]);
                if (d2 < laneTol2 && d2 < bestD2) {
                    bestD2 = d2; bestIdx = i;
                }
            }
        }
        if (bestD2 < std::numeric_limits<float>::infinity()) {
            best.kind = PickKind::Lane;
            best.title = QString("Lane  %1").arg(
                QString::fromStdString(m_ng->lane_ids[bestIdx]));
            best.lines.push_back(QString("kind      %1")
                .arg(laneKindName(m_ng->lane_kind[bestIdx])));
            best.lines.push_back(QString("width     %1 m")
                .arg(m_ng->lane_widths[bestIdx], 0, 'f', 2));
            if (m_snap && !m_snap->lane_attr_rgba.empty()
                && bestIdx * 4 + 3 < m_snap->lane_attr_rgba.size()) {
                best.lines.push_back(QString("attr rgba (%1,%2,%3)")
                    .arg(m_snap->lane_attr_rgba[bestIdx * 4])
                    .arg(m_snap->lane_attr_rgba[bestIdx * 4 + 1])
                    .arg(m_snap->lane_attr_rgba[bestIdx * 4 + 2]));
            }
        }
    }

    // 5) Junctions.
    if (best.kind == PickKind::None) {
        for (std::size_t i = 0; i < m_ng->junction_count(); ++i) {
            const std::uint32_t s = m_ng->junction_offsets[i];
            const std::uint32_t e = m_ng->junction_offsets[i + 1];
            if (e - s < 3) continue;
            if (pointInPoly(fx, fy, &m_ng->junction_points[s * 2], e - s)) {
                best.kind = PickKind::Junction;
                best.title = QString("Junction  %1").arg(
                    QString::fromStdString(m_ng->junction_ids[i]));
                best.lines.push_back(QString("vertices  %1").arg(e - s));
                break;
            }
        }
    }

    m_picked = best;
}

// --------------------------- overlays ---------------------------------

void NetworkView::drawOverlays(QPainter& p) {
    drawLegend(p);
    drawScaleBar(p);
    drawInfoBox(p);
}

void NetworkView::drawScaleBar(QPainter& p) {
    const double ppu = m_cam.pixelsPerUnit();
    if (!(ppu > 0.0)) return;
    const double targetPx = 100.0;
    double meters = targetPx / ppu;
    const double mag = std::pow(10.0, std::floor(std::log10(meters)));
    const double mantissa = meters / mag;
    double nice;
    if      (mantissa < 1.5) nice = 1.0;
    else if (mantissa < 3.5) nice = 2.0;
    else if (mantissa < 7.5) nice = 5.0;
    else                     nice = 10.0;
    meters = nice * mag;
    const double widthPx = meters * ppu;

    QString label;
    if (meters >= 1000.0) label = QString::number(meters / 1000.0, 'g', 3) + " km";
    else if (meters >= 1.0) label = QString::number(meters, 'g', 3) + " m";
    else label = QString::number(meters * 100.0, 'g', 3) + " cm";

    const int margin = 12;
    const QFont f = p.font();
    const QFontMetrics fm(f);
    const int barH = 6;
    const int textH = fm.height();
    const int boxW = static_cast<int>(widthPx) + 2 * margin;
    const int boxH = barH + textH + 8;
    const QRectF box(width() - boxW - margin, height() - boxH - margin, boxW, boxH);
    p.fillRect(box, QColor(0, 0, 0, 160));
    p.setPen(Qt::white);
    const int x0 = static_cast<int>(box.left()) + margin;
    const int y0 = static_cast<int>(box.bottom()) - 4 - barH;
    p.drawLine(x0, y0, x0 + static_cast<int>(widthPx), y0);
    p.drawLine(x0, y0 - 3, x0, y0 + 3);
    p.drawLine(x0 + static_cast<int>(widthPx), y0 - 3,
               x0 + static_cast<int>(widthPx), y0 + 3);
    p.drawText(static_cast<int>(box.left()) + margin,
               static_cast<int>(box.top()) + fm.ascent() + 2, label);
}

void NetworkView::drawLegend(QPainter& p) {
    if (!m_snap || m_snap->lane_attr_label.empty()) return;
    const QString label = QString::fromStdString(m_snap->lane_attr_label);
    const QFontMetrics fm(p.font());
    const int margin = 12;
    const int barW = 160, barH = 10;
    const int textW = fm.horizontalAdvance(label);
    const int boxW = std::max(barW, textW) + 2 * margin;
    const int boxH = barH + fm.height() + 8 + 4;
    const QRectF box(margin, height() - boxH - margin, boxW, boxH);
    p.fillRect(box, QColor(0, 0, 0, 160));
    p.setPen(Qt::white);
    p.drawText(static_cast<int>(box.left()) + margin,
               static_cast<int>(box.top()) + fm.ascent() + 2, label);
    // Red→yellow→green gradient bar.
    QLinearGradient g(box.left() + margin, 0,
                      box.left() + margin + barW, 0);
    g.setColorAt(0.0, QColor(255, 0,   0));
    g.setColorAt(0.5, QColor(255, 255, 0));
    g.setColorAt(1.0, QColor(  0, 255, 0));
    const QRectF bar(box.left() + margin,
                     box.bottom() - barH - 4, barW, barH);
    p.fillRect(bar, g);
}

void NetworkView::drawInfoBox(QPainter& p) {
    if (m_picked.kind == PickKind::None) return;
    const QFontMetrics fm(p.font());
    int maxW = fm.horizontalAdvance(m_picked.title);
    for (const auto& l : m_picked.lines)
        maxW = std::max(maxW, fm.horizontalAdvance(l));
    const int margin = 10;
    const int lineH = fm.height();
    const int boxW = maxW + 2 * margin;
    const int boxH = lineH * (1 + static_cast<int>(m_picked.lines.size())) + 2 * margin;
    const QRectF box(margin, 12 + margin, boxW, boxH);
    p.fillRect(box, QColor(0, 0, 0, 200));
    p.setPen(QColor(255, 200, 80));
    p.drawText(static_cast<int>(box.left()) + margin,
               static_cast<int>(box.top()) + margin + fm.ascent(),
               m_picked.title);
    p.setPen(Qt::white);
    int y = static_cast<int>(box.top()) + margin + fm.ascent() + lineH;
    for (const auto& l : m_picked.lines) {
        p.drawText(static_cast<int>(box.left()) + margin, y, l);
        y += lineH;
    }
}
