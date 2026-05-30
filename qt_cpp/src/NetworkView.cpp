#include "NetworkView.h"

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QResizeEvent>
#include <QtGui/QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

#include "NetworkOverlayWidget.h"
#include "layers/LayerBuilders.h"
#include "layers/PersonLayerRhi.h"
#include "layers/POILayerRhi.h"
#include "layers/TLSLayerRhi.h"
#include "layers/VehicleLayerRhi.h"
#include "rhi_compat/rhi_compat.h"
#include "sim/NetworkGeometry.h"

NetworkView::NetworkView(QWidget* parent)
    : rhi_compat::RhiWidgetBase(parent) {
    rhi_compat::selectOpenGL(this);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    m_overlay = new NetworkOverlayWidget(this);
    m_overlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_overlay->raise();

    // Single-shot timer used by the max-fps cap to defer snapshot-driven
    // updates that would arrive sooner than 1000 / m_maxFps ms apart.
    m_fpsCapTimer = new QTimer(this);
    m_fpsCapTimer->setSingleShot(true);
    connect(m_fpsCapTimer, &QTimer::timeout, this,
            [this]() { update(); });
}

NetworkView::~NetworkView() = default;

void NetworkView::releaseResources() {
    m_passTris.release();
    m_passStrip.release();
    m_detector.release(); m_stoppingPlace.release(); m_stopLine.release();
    m_pedSidewalk.release(); m_pedWalk.release();
    m_railSleepers.release(); m_railRails.release();
    m_polygonFills.release(); m_polygonOutlines.release();
    m_netJunctions.release(); m_netLaneStrip.release();
    m_edgeColorStrip.release();
    if (m_vehicleLayer) m_vehicleLayer->release();
    if (m_personLayer)  m_personLayer->release();
    if (m_poiLayer)     m_poiLayer->release();
    if (m_tlsLayer)     m_tlsLayer->release();
}

void NetworkView::initialize(QRhiCommandBuffer*) {
    QRhi* r = rhi();
    if (!r) return;
    QRhiRenderTarget* rt = renderTarget();
    if (!rt) return;
    QRhiRenderPassDescriptor* rp = rt->renderPassDescriptor();
    const int sc = rt->sampleCount();

    // Shared passes (one per topology). Re-init is cheap if rhi unchanged.
    m_passTris.init(r, rp, sc, QRhiGraphicsPipeline::Triangles);
    m_passStrip.init(r, rp, sc, QRhiGraphicsPipeline::TriangleStrip);

    // Static buffers — initialize() is idempotent if rhi unchanged.
    m_detector.initialize(r);       m_stoppingPlace.initialize(r);
    m_stopLine.initialize(r);
    m_pedSidewalk.initialize(r);    m_pedWalk.initialize(r);
    m_railSleepers.initialize(r);   m_railRails.initialize(r);
    m_polygonFills.initialize(r);   m_polygonOutlines.initialize(r);
    m_netJunctions.initialize(r);   m_netLaneStrip.initialize(r);
    m_edgeColorStrip.initialize(r);

    if (!m_vehicleLayer) m_vehicleLayer = std::make_unique<VehicleLayerRhi>();
    if (!m_personLayer)  m_personLayer  = std::make_unique<PersonLayerRhi>();
    if (!m_poiLayer)     m_poiLayer     = std::make_unique<POILayerRhi>();
    if (!m_tlsLayer)     m_tlsLayer     = std::make_unique<TLSLayerRhi>();

    // Apply any shape selection made by the UI before the layer existed.
    m_vehicleLayer->setShape(m_pendingVehicleShape);
    m_vehicleLayer->initialize(r, rp, sc);
    m_personLayer ->initialize(r, rp, sc);
    m_poiLayer    ->initialize(r, rp, sc);
    m_tlsLayer    ->initialize(r, rp, sc);

    if (m_ng) {
        uploadStaticGeometry();
        m_poiLayer->setGeometry(m_ng);
        m_tlsLayer->setGeometry(m_ng);
    }
    if (m_pendingSnap) {
        m_snap = m_pendingSnap;
        m_vehicleLayer->setSnapshot(m_snap);
        m_personLayer ->setSnapshot(m_snap);
        m_tlsLayer    ->setSnapshot(m_snap);
        m_edgeColorVisible = !m_snap->lane_attr_rgba.empty();
        if (m_edgeColorVisible) {
            uploadEdgeColors();
        }
        m_snapDirty = false;
    }
}

void NetworkView::uploadStaticGeometry() {
    if (!m_ng) return;
    using namespace layer_builders;
    m_detector.setVertices(buildDetectorVerts(*m_ng));
    m_stoppingPlace.setVertices(buildStoppingPlaceVerts(*m_ng));
    m_stopLine.setVertices(buildStopLineVerts(*m_ng));
    auto ped = buildPedAreaVerts(*m_ng);
    m_pedSidewalk.setVertices(std::move(ped.sidewalk));
    m_pedWalk.setVertices(std::move(ped.walkArea));
    auto rail = buildRailVerts(*m_ng);
    m_railSleepers.setVertices(std::move(rail.sleepers));
    m_railRails.setVertices(std::move(rail.rails));
    auto poly = buildPolygonVerts(*m_ng);
    m_polygonFills.setVertices(std::move(poly.fills));
    m_polygonOutlines.setVertices(std::move(poly.outlines));
    auto net = buildNetworkVerts(*m_ng);
    m_netLaneStrip.setVertices(std::move(net.laneStrip));
    m_netJunctions.setVertices(std::move(net.juncTris));
    auto edge = buildEdgeColorVerts(*m_ng);
    m_laneFirst = std::move(edge.laneFirst);
    m_laneCount = std::move(edge.laneCount);
    m_edgeColorStrip.setVertices(std::move(edge.strip));
}

void NetworkView::uploadEdgeColors() {
    if (!m_snap || m_snap->lane_attr_rgba.empty()) {
        m_edgeColorVisible = false;
        return;
    }
    auto& verts = m_edgeColorStrip.mutableVertices();
    if (verts.empty() || m_laneFirst.size() * 4 != m_snap->lane_attr_rgba.size()) {
        m_edgeColorVisible = false;
        return;
    }
    const auto& rgba = m_snap->lane_attr_rgba;
    for (std::size_t i = 0; i < m_laneFirst.size(); ++i) {
        const std::uint32_t f = m_laneFirst[i];
        const std::uint32_t c = m_laneCount[i];
        const quint8 r = rgba[i * 4 + 0];
        const quint8 g = rgba[i * 4 + 1];
        const quint8 b = rgba[i * 4 + 2];
        const quint8 a = rgba[i * 4 + 3];
        for (std::uint32_t v = f; v < f + c; ++v) {
            verts[v].r = r; verts[v].g = g; verts[v].b = b; verts[v].a = a;
        }
    }
    m_edgeColorStrip.markDirty();
    m_edgeColorVisible = true;
}

void NetworkView::render(QRhiCommandBuffer* cb) {
    QRhi* r = rhi();
    QRhiRenderTarget* rt = renderTarget();
    if (!r || !rt) return;

    // Track viewport in pixels (not logical px) — matches QRhi convention.
    const QSize px = rt->pixelSize();
    m_cam.setViewport(px.width(), px.height());
    if (m_hasBounds && m_needsInitialFit) {
        m_cam.fitBounds(m_minX, m_minY, m_maxX, m_maxY);
        m_needsInitialFit = false;
    }
    const auto proj = m_cam.projection();
    const float* pf = proj.data();

    // Apply pending data updates.
    if (m_geomDirty) {
        uploadStaticGeometry();
        m_poiLayer->setGeometry(m_ng);
        m_tlsLayer->setGeometry(m_ng);
        m_geomDirty = false;
        m_edgeColorVisible = false;
    }
    if (m_snapDirty) {
        m_snap = m_pendingSnap;
        if (m_vehicleLayer) m_vehicleLayer->setSnapshot(m_snap);
        if (m_personLayer)  m_personLayer ->setSnapshot(m_snap);
        if (m_tlsLayer)     m_tlsLayer    ->setSnapshot(m_snap);
        uploadEdgeColors();
        m_snapDirty = false;
        // Signal the worker that the GUI is ready for the next frame.
        if (m_renderPendingFlag)
            m_renderPendingFlag->store(0, std::memory_order_release);
    }

    // Stage uploads.
    QRhiResourceUpdateBatch* batch = r->nextResourceUpdateBatch();
    m_passTris.setProjection(pf);
    m_passStrip.setProjection(pf);
    m_passTris.uploadUbo(batch);
    m_passStrip.uploadUbo(batch);

    m_detector.resourceUpdate(batch);
    m_stoppingPlace.resourceUpdate(batch);
    m_stopLine.resourceUpdate(batch);
    m_pedSidewalk.resourceUpdate(batch);
    m_pedWalk.resourceUpdate(batch);
    m_railSleepers.resourceUpdate(batch);
    m_railRails.resourceUpdate(batch);
    m_polygonFills.resourceUpdate(batch);
    m_polygonOutlines.resourceUpdate(batch);
    m_netJunctions.resourceUpdate(batch);
    m_netLaneStrip.resourceUpdate(batch);
    m_edgeColorStrip.resourceUpdate(batch);

    if (m_vehicleLayer) {
        m_vehicleLayer->setProjection(pf);
        // Mirror ecal `vehicleMinPixels` ≈ 6 px. Convert px → world meters
        // via the camera's current pixels-per-meter. Width clamp is half as
        // big as length so motorbikes/bikes don't get fattened too much.
        const double ppu = m_cam.pixelsPerUnit();
        const float minLen = ppu > 1e-6 ? float(6.0 / ppu) : 0.f;
        const float minWid = minLen * 0.5f;
        m_vehicleLayer->setMinSize(minLen, minWid);
        m_vehicleLayer->resourceUpdate(batch);
    }
    if (m_personLayer)  { m_personLayer ->setProjection(pf); m_personLayer ->resourceUpdate(batch); }
    if (m_poiLayer)     { m_poiLayer    ->setProjection(pf); m_poiLayer    ->resourceUpdate(batch); }
    if (m_tlsLayer)     { m_tlsLayer    ->setProjection(pf); m_tlsLayer    ->resourceUpdate(batch); }

    // One render pass.
    const QColor clear(13, 13, 18);
    cb->beginPass(rt, clear, { 1.0f, 0 }, batch);
    cb->setViewport({ 0, 0, float(px.width()), float(px.height()) });

    // Draw order: junctions → lanes → edge-color overlay → ped → rails →
    // polygons (fills under outlines) → POIs → stopping places → detectors →
    // TLS → stop lines → vehicles → persons.
    m_netJunctions.render(cb, m_passTris);
    m_netLaneStrip.render(cb, m_passStrip);
    if (m_edgeColorVisible && m_layerEdgeDataVisible) m_edgeColorStrip.render(cb, m_passStrip);
    m_pedSidewalk.render(cb, m_passTris);
    m_pedWalk.render(cb, m_passTris);
    m_railSleepers.render(cb, m_passTris);
    m_railRails.render(cb, m_passTris);
    m_polygonFills.render(cb, m_passTris);
    m_polygonOutlines.render(cb, m_passTris);
    if (m_poiLayer) m_poiLayer->render(cb);
    m_stoppingPlace.render(cb, m_passTris);
    m_detector.render(cb, m_passTris);
    if (m_tlsLayer && m_layerTLSVisible) m_tlsLayer->render(cb);
    if (m_layerTLSVisible) m_stopLine.render(cb, m_passTris);
    if (m_vehicleLayer && m_layerVehiclesVisible) m_vehicleLayer->render(cb);
    if (m_personLayer && m_layerAgentsVisible)    m_personLayer ->render(cb);

    cb->endPass();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_lastUpdateMs = now;
    if (m_fpsWindowStartMs == 0) m_fpsWindowStartMs = now;
    ++m_fpsFrames;
    const qint64 dt = now - m_fpsWindowStartMs;
    if (dt >= 500) {
        const double fps = 1000.0 * m_fpsFrames / static_cast<double>(dt);
        emit fpsUpdated(fps);
        m_fpsFrames = 0;
        m_fpsWindowStartMs = now;
    }

    if (m_overlay) m_overlay->update();
}

// ---------------------------- slots / events -------------------------------

void NetworkView::setNetwork(std::shared_ptr<NetworkGeometry> ng) {
    m_ng = std::move(ng);
    m_picked = Picked{};
    if (m_ng) {
        m_hasBounds = true;
        m_needsInitialFit = true;
        m_minX = m_ng->min_x; m_minY = m_ng->min_y;
        m_maxX = m_ng->max_x; m_maxY = m_ng->max_y;
    }
    m_geomDirty = true;
    update();
}

void NetworkView::setSnapshot(SimSnapshotPtr snap) {
    m_pendingSnap = std::move(snap);
    m_snapDirty = true;
    if (!m_followId.isEmpty() && m_pendingSnap) {
        const std::string fid = m_followId.toStdString();
        bool found = false;
        const auto* vpos = reinterpret_cast<const double*>(
            m_pendingSnap->veh_positions.data());
        for (std::size_t i = 0; i < m_pendingSnap->vehicle_count(); ++i) {
            if (m_pendingSnap->ids[i] == fid) {
                m_cam.setCenter(vpos[i * 3 + 0], vpos[i * 3 + 1]);
                found = true;
                break;
            }
        }
        if (!found) m_followId.clear();
    }
    // Cap-throttled update: if we just painted less than (1000/maxFps) ms
    // ago, defer the repaint via the one-shot timer.  The freshest snapshot
    // is held in m_pendingSnap, so a late firing render() still grabs the
    // latest data — older deferred frames coalesce naturally.
    if (m_maxFps <= 0) {
        update();
    } else {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 minIntervalMs = 1000 / m_maxFps;
        const qint64 since = now - m_lastUpdateMs;
        if (since >= minIntervalMs) {
            update();
        } else if (!m_fpsCapTimer->isActive()) {
            m_fpsCapTimer->start(static_cast<int>(minIntervalMs - since));
        }
    }
}

void NetworkView::setMaxFps(int fps) {
    m_maxFps = fps > 0 ? fps : 0;
    if (m_maxFps == 0 && m_fpsCapTimer) m_fpsCapTimer->stop();
}

void NetworkView::setFollowSelected() {
    if (m_picked.kind == PickKind::Vehicle && !m_picked.id.isEmpty()) {
        m_followId = m_picked.id;
        update();
    }
}

void NetworkView::clearFollow() {
    if (m_followId.isEmpty()) return;
    m_followId.clear();
    update();
}

void NetworkView::resetView() {
    if (!m_hasBounds) return;
    m_cam.fitBounds(m_minX, m_minY, m_maxX, m_maxY);
    update();
}

void NetworkView::setVehiclesVisible(bool on)  { m_layerVehiclesVisible = on; update(); }
void NetworkView::setAgentsVisible  (bool on)  { m_layerAgentsVisible   = on; update(); }
void NetworkView::setTLSVisible     (bool on)  { m_layerTLSVisible      = on; update(); }
void NetworkView::setEdgeDataVisible(bool on)  { m_layerEdgeDataVisible = on; update(); }

void NetworkView::setVehicleShape(int shape) {
    using S = VehicleLayerRhi::Shape;
    S s = S::Rectangle;
    switch (shape) {
        case 1: s = S::Triangle; break;
        case 2: s = S::Car;      break;
        case 3: s = S::Circle;   break;
        default: s = S::Rectangle;
    }
    m_pendingVehicleShape = s;
    if (m_vehicleLayer) m_vehicleLayer->setShape(s);
    update();
}

void NetworkView::resizeEvent(QResizeEvent* e) {
    rhi_compat::RhiWidgetBase::resizeEvent(e);
    if (m_overlay) m_overlay->setGeometry(rect());
}

void NetworkView::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_mouseDown = true; m_didDrag = false;
        m_downPos = e->pos(); m_lastMouse = e->pos();
    }
}

void NetworkView::mouseMoveEvent(QMouseEvent* e) {
    // Emit world-space coords for the status bar. Done on every move (not
    // just while panning) so the user always sees the cursor location.
    {
        const double dpr = devicePixelRatioF();
        double wx = 0, wy = 0;
        m_cam.pixelToWorld(e->position().x() * dpr,
                           e->position().y() * dpr, wx, wy);
        emit cursorWorldPos(wx, wy);
    }
    if (m_mouseDown) {
        const QPoint d = e->pos() - m_lastMouse;
        m_lastMouse = e->pos();
        if (!m_didDrag && (e->pos() - m_downPos).manhattanLength() > 4) {
            m_didDrag = true; m_panning = true;
        }
        if (m_panning) {
            // Camera uses pixel deltas in widget logical px. Convert from
            // device px below in zoomAtPixel/pixelToWorld too.
            m_cam.panPixels(d.x() * devicePixelRatioF(),
                            d.y() * devicePixelRatioF());
            update();
        }
    }
}

void NetworkView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const bool wasClick = m_mouseDown && !m_didDrag;
    m_mouseDown = false; m_panning = false;
    if (wasClick) {
        const double dpr = devicePixelRatioF();
        pickAt(e->position().x() * dpr, e->position().y() * dpr);
        update();
    }
}

void NetworkView::wheelEvent(QWheelEvent* e) {
    const double steps  = e->angleDelta().y() / 120.0;
    const double factor = std::pow(1.2, steps);
    const QPointF pos = e->position();
    const double dpr = devicePixelRatioF();
    m_cam.zoomAtPixel(pos.x() * dpr, pos.y() * dpr, factor);
    update();
    e->accept();
}

// ---------------------------- picking (unchanged) --------------------------

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
    switch (k) { case 0: return "road"; case 1: return "rail";
                 case 2: return "sidewalk"; case 3: return "walkingarea/crossing";
                 case 4: return "internal"; }
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
    const float pickRadius = static_cast<float>(8.0 / ppu);
    const float pickR2 = pickRadius * pickRadius;

    Picked best;
    if (m_snap) {
        const auto* vpos = reinterpret_cast<const double*>(m_snap->veh_positions.data());
        const auto* vang = reinterpret_cast<const float*>(m_snap->veh_angles.data());
        float bestD2 = std::numeric_limits<float>::infinity();
        std::size_t bestIdx = 0;
        for (std::size_t i = 0; i < m_snap->vehicle_count(); ++i) {
            const float dx = static_cast<float>(vpos[i * 3 + 0]) - fx;
            const float dy = static_cast<float>(vpos[i * 3 + 1]) - fy;
            const float d2 = dx * dx + dy * dy;
            const float tol2 = std::max(pickR2, 4.0f);
            if (d2 < tol2 && d2 < bestD2) { bestD2 = d2; bestIdx = i; }
        }
        if (bestD2 < std::numeric_limits<float>::infinity()) {
            best.kind = PickKind::Vehicle;
            best.id = QString::fromStdString(m_snap->ids[bestIdx]);
            best.title = QString("Vehicle  %1").arg(best.id);
            // Type id (looked up via the registry index that came with the
            // snapshot).
            if (bestIdx < m_snap->veh_type_indices.size()) {
                const std::uint32_t ti = m_snap->veh_type_indices[bestIdx];
                if (ti < m_snap->type_ids.size() && !m_snap->type_ids[ti].empty()) {
                    best.lines.push_back(QString("type      %1")
                        .arg(QString::fromStdString(m_snap->type_ids[ti])));
                }
            }
            // Speed (always populated by libsumo::Batch).
            if (bestIdx * sizeof(float) + sizeof(float) <= m_snap->veh_speeds.size()) {
                const auto* sp = reinterpret_cast<const float*>(m_snap->veh_speeds.data());
                best.lines.push_back(QString("speed     %1 m/s  (%2 km/h)")
                    .arg(sp[bestIdx], 0, 'f', 2)
                    .arg(sp[bestIdx] * 3.6f, 0, 'f', 1));
            }
            best.lines.push_back(QString("position  %1, %2 m")
                .arg(vpos[bestIdx * 3 + 0], 0, 'f', 1)
                .arg(vpos[bestIdx * 3 + 1], 0, 'f', 1));
            best.lines.push_back(QString("heading   %1°").arg(vang[bestIdx], 0, 'f', 1));
        }
        if (best.kind == PickKind::None) {
            const auto* ppos = reinterpret_cast<const double*>(m_snap->agent_positions.data());
            bestD2 = std::numeric_limits<float>::infinity();
            for (std::size_t i = 0; i < m_snap->person_count(); ++i) {
                const float dx = static_cast<float>(ppos[i * 3 + 0]) - fx;
                const float dy = static_cast<float>(ppos[i * 3 + 1]) - fy;
                const float d2 = dx * dx + dy * dy;
                if (d2 < pickR2 && d2 < bestD2) { bestD2 = d2; bestIdx = i; }
            }
            if (bestD2 < std::numeric_limits<float>::infinity()) {
                best.kind = PickKind::Person;
                best.title = QString("Person  %1").arg(
                    QString::fromStdString(m_snap->person_ids[bestIdx]));
                best.lines.push_back(QString("position  %1, %2 m")
                    .arg(ppos[bestIdx * 3 + 0], 0, 'f', 1)
                    .arg(ppos[bestIdx * 3 + 1], 0, 'f', 1));
            }
        }
    }
    if (best.kind == PickKind::None) {
        const std::size_t n = m_ng->tls_marker_count();
        float bestD2 = std::numeric_limits<float>::infinity();
        std::size_t bestIdx = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const float dx = m_ng->tls_x[i] - fx, dy = m_ng->tls_y[i] - fy;
            const float d2 = dx * dx + dy * dy;
            const float tol2 = std::max(pickR2, 1.44f);
            if (d2 < tol2 && d2 < bestD2) { bestD2 = d2; bestIdx = i; }
        }
        if (bestD2 < std::numeric_limits<float>::infinity()) {
            best.kind = PickKind::TLS;
            best.title = QString("Traffic light  %1")
                .arg(QString::fromStdString(m_ng->tls_ids[bestIdx]));
            best.lines.push_back(QString("link index  %1")
                .arg(m_ng->tls_state_index[bestIdx]));
            if (m_snap) {
                auto it = m_snap->tls_states.find(m_ng->tls_ids[bestIdx]);
                if (it != m_snap->tls_states.end()) {
                    const auto& st = it->second;
                    const std::uint32_t li = m_ng->tls_state_index[bestIdx];
                    best.lines.push_back(QString("state       %1")
                        .arg(QString::fromStdString(st)));
                    if (li < st.size())
                        best.lines.push_back(QString("this link   %1").arg(QChar(st[li])));
                }
            }
        }
    }
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
                break;
            }
        }
    }
    if (best.kind == PickKind::None) {
        float bestD2 = std::numeric_limits<float>::infinity();
        std::size_t bestIdx = 0;
        for (std::size_t i = 0; i < m_ng->poi_count(); ++i) {
            const float dx = m_ng->poi_x[i] - fx, dy = m_ng->poi_y[i] - fy;
            const float d2 = dx * dx + dy * dy;
            const float tol2 = std::max(pickR2, 3.24f);
            if (d2 < tol2 && d2 < bestD2) { bestD2 = d2; bestIdx = i; }
        }
        if (bestD2 < std::numeric_limits<float>::infinity()) {
            best.kind = PickKind::POI;
            best.title = QString("POI  %1").arg(QString::fromStdString(m_ng->poi_ids[bestIdx]));
            if (!m_ng->poi_types[bestIdx].empty())
                best.lines.push_back(QString("type   %1")
                    .arg(QString::fromStdString(m_ng->poi_types[bestIdx])));
        }
    }
    if (best.kind == PickKind::None) {
        float bestD2 = std::numeric_limits<float>::infinity();
        std::size_t bestIdx = 0;
        for (std::size_t i = 0; i < m_ng->stop_count(); ++i) {
            const float dx = m_ng->stop_x[i] - fx, dy = m_ng->stop_y[i] - fy;
            const float d2 = dx * dx + dy * dy;
            const float tol = std::max(m_ng->stop_len[i], m_ng->stop_w[i]) * 0.5f + 1.0f;
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
        }
    }
    if (best.kind == PickKind::None) {
        float bestD2 = std::numeric_limits<float>::infinity();
        std::size_t bestIdx = 0;
        for (std::size_t i = 0; i < m_ng->det_count(); ++i) {
            const float dx = m_ng->det_x[i] - fx, dy = m_ng->det_y[i] - fy;
            const float d2 = dx * dx + dy * dy;
            const float tol = std::max(m_ng->det_len[i], 2.0f) * 0.5f + 1.0f;
            const float tol2 = std::max(pickR2, tol * tol);
            if (d2 < tol2 && d2 < bestD2) { bestD2 = d2; bestIdx = i; }
        }
        if (bestD2 < std::numeric_limits<float>::infinity()) {
            best.kind = PickKind::Detector;
            const char* k = "Detector";
            switch (m_ng->det_kind[bestIdx]) {
                case 0: k = "Induction loop"; break;
                case 1: k = "Lane-area detector"; break;
                case 2: k = "MultiEntryExit (entry)"; break;
                case 3: k = "MultiEntryExit (exit)"; break;
            }
            best.title = QString("%1  %2").arg(k,
                QString::fromStdString(m_ng->det_ids[bestIdx]));
        }
    }
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
                if (d2 < laneTol2 && d2 < bestD2) { bestD2 = d2; bestIdx = i; }
            }
        }
        if (bestD2 < std::numeric_limits<float>::infinity()) {
            best.kind = PickKind::Lane;
            best.title = QString("Lane  %1")
                .arg(QString::fromStdString(m_ng->lane_ids[bestIdx]));
            best.lines.push_back(QString("kind      %1")
                .arg(laneKindName(m_ng->lane_kind[bestIdx])));
            best.lines.push_back(QString("width     %1 m")
                .arg(m_ng->lane_widths[bestIdx], 0, 'f', 2));
        }
    }
    if (best.kind == PickKind::None) {
        for (std::size_t i = 0; i < m_ng->junction_count(); ++i) {
            const std::uint32_t s = m_ng->junction_offsets[i];
            const std::uint32_t e = m_ng->junction_offsets[i + 1];
            if (e - s < 3) continue;
            if (pointInPoly(fx, fy, &m_ng->junction_points[s * 2], e - s)) {
                best.kind = PickKind::Junction;
                best.title = QString("Junction  %1")
                    .arg(QString::fromStdString(m_ng->junction_ids[i]));
                break;
            }
        }
    }
    m_picked = best;
}
