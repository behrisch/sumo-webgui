#include "LayerBuilders.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "sim/NetworkGeometry.h"

namespace layer_builders {

namespace {

inline TrisColorVertex V(float x, float y, quint8 r, quint8 g, quint8 b, quint8 a) {
    return TrisColorVertex{x, y, r, g, b, a};
}

inline void pushQuadLocal(std::vector<TrisColorVertex>& v,
                     float ax, float ay, float bx, float by,
                     float cx, float cy, float dx, float dy,
                     quint8 r, quint8 g, quint8 b, quint8 a) {
    const TrisColorVertex A = V(ax, ay, r, g, b, a);
    const TrisColorVertex B = V(bx, by, r, g, b, a);
    const TrisColorVertex C = V(cx, cy, r, g, b, a);
    const TrisColorVertex D = V(dx, dy, r, g, b, a);
    v.insert(v.end(), {A, B, C, A, C, D});
}

// Extrude one segment into 2 triangles, independent (no strip stitching).
inline void pushSegmentQuad(std::vector<TrisColorVertex>& out,
                            float x0, float y0, float x1, float y1,
                            float halfW,
                            quint8 r, quint8 g, quint8 b, quint8 a) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float L = std::sqrt(dx * dx + dy * dy);
    if (L < 1e-6f) return;
    const float nx = -dy / L * halfW;
    const float ny =  dx / L * halfW;
    pushQuadLocal(out,
        x0 + nx, y0 + ny,
        x0 - nx, y0 - ny,
        x1 - nx, y1 - ny,
        x1 + nx, y1 + ny,
        r, g, b, a);
}

// Extrude polyline into a left/right strip (2 verts per polyline point).
// Uses averaged adjacent normals at interior vertices, same logic as the
// OpenGL NetworkLayer. Output: pairs of vertices (left, right) per pt.
void extrudePolylineStrip(const float* pts, std::size_t nPts, float width,
                          quint8 r, quint8 g, quint8 b, quint8 a,
                          std::vector<TrisColorVertex>& out) {
    if (nPts < 2 || width <= 0.0f) return;
    const float halfW = 0.5f * width;
    auto segN = [](float x0, float y0, float x1, float y1,
                   float& nx, float& ny) {
        const float dx = x1 - x0, dy = y1 - y0;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-6f) { nx = 0; ny = 0; return; }
        nx = -dy / len; ny = dx / len;
    };
    auto push = [&](float x, float y, float nx, float ny) {
        out.push_back(V(x + nx * halfW, y + ny * halfW, r, g, b, a));
        out.push_back(V(x - nx * halfW, y - ny * halfW, r, g, b, a));
    };
    float pnx, pny;
    segN(pts[0], pts[1], pts[2], pts[3], pnx, pny);
    push(pts[0], pts[1], pnx, pny);
    for (std::size_t i = 1; i + 1 < nPts; ++i) {
        const std::size_t k = i * 2;
        float n1x, n1y, n2x, n2y;
        segN(pts[k - 2], pts[k - 1], pts[k],     pts[k + 1], n1x, n1y);
        segN(pts[k],     pts[k + 1], pts[k + 2], pts[k + 3], n2x, n2y);
        float ax = n1x + n2x, ay = n1y + n2y;
        const float al = std::sqrt(ax * ax + ay * ay);
        if (al < 1e-4f) { ax = n2x; ay = n2y; } else { ax /= al; ay /= al; }
        push(pts[k], pts[k + 1], ax, ay);
        pnx = n2x; pny = n2y;
    }
    const std::size_t lk = (nPts - 1) * 2;
    push(pts[lk], pts[lk + 1], pnx, pny);
}

// Append a degenerate transition before appending a new strip. Duplicates
// the previous strip's last vertex and the next strip's first vertex.
// Caller must supply both vertices (with whatever color).
inline void appendStripBridge(std::vector<TrisColorVertex>& out,
                              const TrisColorVertex& prevLast,
                              const TrisColorVertex& nextFirst) {
    out.push_back(prevLast);
    out.push_back(nextFirst);
}

}  // namespace

// ---------------------------------------------------------------------------
std::vector<TrisColorVertex> buildDetectorVerts(const NetworkGeometry& ng) {
    constexpr float kPointBar = 0.5f;
    constexpr float kBandHalfWidth = 1.6f;
    std::vector<TrisColorVertex> verts;
    const std::size_t n = ng.det_count();
    verts.reserve(n * 6);
    for (std::size_t i = 0; i < n; ++i) {
        const float cx = ng.det_x[i], cy = ng.det_y[i];
        const float dx = ng.det_dx[i], dy = ng.det_dy[i];
        const float len = ng.det_len[i];
        const float px = -dy, py = dx;
        quint8 r, g, b, a; float halfL, halfW;
        switch (ng.det_kind[i]) {
            case 0: r=240; g=60;  b=200; a=230; halfL=kPointBar;                 halfW=kBandHalfWidth;       break;
            case 1: r=240; g=60;  b=200; a=150; halfL=0.5f*std::max(0.5f, len);  halfW=kBandHalfWidth*0.6f;  break;
            case 2: r=60;  g=220; b=90;  a=240; halfL=kPointBar;                 halfW=kBandHalfWidth;       break;
            default:r=230; g=50;  b=50;  a=240; halfL=kPointBar;                 halfW=kBandHalfWidth;       break;
        }
        const float hx = dx*halfL, hy = dy*halfL;
        const float kx = px*halfW, ky = py*halfW;
        pushQuadLocal(verts,
            cx - hx - kx, cy - hy - ky,
            cx + hx - kx, cy + hy - ky,
            cx + hx + kx, cy + hy + ky,
            cx - hx + kx, cy - hy + ky,
            r, g, b, a);
    }
    return verts;
}

// ---------------------------------------------------------------------------
std::vector<TrisColorVertex> buildStoppingPlaceVerts(const NetworkGeometry& ng) {
    std::vector<TrisColorVertex> verts;
    const std::size_t n = ng.stop_count();
    verts.reserve(n * 6);
    for (std::size_t i = 0; i < n; ++i) {
        const float cx = ng.stop_x[i], cy = ng.stop_y[i];
        const float dx = ng.stop_dx[i], dy = ng.stop_dy[i];
        const float len = ng.stop_len[i];
        const float w   = ng.stop_w[i];
        const float px = -dy, py = dx;
        const float bandW = std::max(1.0f, 0.6f * w);
        const float offset = 0.5f * w + 0.5f * bandW;
        const float ox = -px * offset, oy = -py * offset;
        const float hx = 0.5f * dx * len, hy = 0.5f * dy * len;
        const float kx = -px * 0.5f * bandW, ky = -py * 0.5f * bandW;
        quint8 r = 0, g = 0, b = 0, a = 220;
        switch (ng.stop_kind[i]) {
            case 0: r = 40;  g = 120; b = 220; break;
            case 1: r = 80;  g = 220; b = 220; break;
            case 2: r = 230; g = 150; b = 30;  break;
        }
        pushQuadLocal(verts,
            cx + ox - hx - kx, cy + oy - hy - ky,
            cx + ox + hx - kx, cy + oy + hy - ky,
            cx + ox + hx + kx, cy + oy + hy + ky,
            cx + ox - hx + kx, cy + oy - hy + ky,
            r, g, b, a);
    }
    return verts;
}

// ---------------------------------------------------------------------------
std::vector<TrisColorVertex> buildStopLineVerts(const NetworkGeometry& ng) {
    // Matches kBarLen in TLSLayerRhi.cpp — both kinds of "lane-end bar"
    // (uncontrolled stop lines and TLS signals) share the same along-lane
    // thickness so they read as one visual primitive.
    constexpr float kThickness = 0.9f;
    const std::size_t n = ng.stopline_count();
    std::vector<TrisColorVertex> verts;
    verts.reserve(n * 6);
    for (std::size_t i = 0; i < n; ++i) {
        const float cx = ng.stopline_x[i], cy = ng.stopline_y[i];
        const float dx = ng.stopline_dx[i], dy = ng.stopline_dy[i];
        const float w  = ng.stopline_w[i];
        const float px = -dy, py = dx;
        const float hw = 0.5f * w, ht = 0.5f * kThickness;
        pushQuadLocal(verts,
            cx - dx*ht - px*hw, cy - dy*ht - py*hw,
            cx + dx*ht - px*hw, cy + dy*ht - py*hw,
            cx + dx*ht + px*hw, cy + dy*ht + py*hw,
            cx - dx*ht + px*hw, cy - dy*ht + py*hw,
            255, 255, 255, 255);
    }
    return verts;
}

// ---------------------------------------------------------------------------
PedAreaVerts buildPedAreaVerts(const NetworkGeometry& ng) {
    PedAreaVerts out;
    const quint8 sidewalkR=140, sidewalkG=128, sidewalkB=102, sidewalkA=255;
    const quint8 walkR=166, walkG=166, walkB=158, walkA=255;
    for (std::size_t i = 0; i < ng.lane_count(); ++i) {
        const auto kind = ng.lane_kind[i];
        if (kind != 2 && kind != 3) continue;
        const std::uint32_t s = ng.lane_offsets[i];
        const std::uint32_t e = ng.lane_offsets[i + 1];
        if (e - s < 2) continue;
        const float halfW = 0.5f * ng.lane_widths[i];
        auto& bucket = (kind == 2) ? out.sidewalk : out.walkArea;
        const quint8 r = (kind == 2) ? sidewalkR : walkR;
        const quint8 g = (kind == 2) ? sidewalkG : walkG;
        const quint8 b = (kind == 2) ? sidewalkB : walkB;
        const quint8 a = (kind == 2) ? sidewalkA : walkA;
        const float* pts = &ng.lane_points[s * 2];
        for (std::uint32_t k = 0; k + 1 < (e - s); ++k) {
            pushSegmentQuad(bucket,
                pts[k*2], pts[k*2+1], pts[(k+1)*2], pts[(k+1)*2+1],
                halfW, r, g, b, a);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
RailVerts buildRailVerts(const NetworkGeometry& ng) {
    constexpr float kGauge = 1.435f;
    constexpr float kRailHalfWidth   = 0.10f;
    constexpr float kSleeperHalfLen  = 1.20f;
    constexpr float kSleeperHalfWid  = 0.20f;
    constexpr float kSleeperSpacing  = 3.5f;
    const quint8 sleeperR = 82,  sleeperG = 56,  sleeperB = 38,  sleeperA = 255;
    const quint8 railR    = 217, railG    = 217, railB    = 230, railA    = 255;
    const float halfGauge = 0.5f * kGauge;

    RailVerts out;
    auto pushSleepers = [&](const float* pts, std::size_t nPts) {
        float distSinceLast = kSleeperSpacing * 0.5f;
        for (std::size_t i = 0; i + 1 < nPts; ++i) {
            const float x0=pts[i*2],   y0=pts[i*2+1];
            const float x1=pts[i*2+2], y1=pts[i*2+3];
            float ex = x1-x0, ey = y1-y0;
            const float segLen = std::sqrt(ex*ex + ey*ey);
            if (segLen < 1e-4f) continue;
            const float ux = ex/segLen, uy = ey/segLen;
            const float nx = -uy, ny = ux;
            float along = kSleeperSpacing - distSinceLast;
            while (along <= segLen) {
                const float cx = x0 + ux * along;
                const float cy = y0 + uy * along;
                pushQuadLocal(out.sleepers,
                    cx + nx*kSleeperHalfLen - ux*kSleeperHalfWid,
                    cy + ny*kSleeperHalfLen - uy*kSleeperHalfWid,
                    cx + nx*kSleeperHalfLen + ux*kSleeperHalfWid,
                    cy + ny*kSleeperHalfLen + uy*kSleeperHalfWid,
                    cx - nx*kSleeperHalfLen + ux*kSleeperHalfWid,
                    cy - ny*kSleeperHalfLen + uy*kSleeperHalfWid,
                    cx - nx*kSleeperHalfLen - ux*kSleeperHalfWid,
                    cy - ny*kSleeperHalfLen - uy*kSleeperHalfWid,
                    sleeperR, sleeperG, sleeperB, sleeperA);
                along += kSleeperSpacing;
            }
            distSinceLast = segLen - (along - kSleeperSpacing);
        }
    };
    auto pushRail = [&](const float* pts, std::size_t nPts, float offset) {
        for (std::size_t i = 0; i + 1 < nPts; ++i) {
            const float x0=pts[i*2],   y0=pts[i*2+1];
            const float x1=pts[i*2+2], y1=pts[i*2+3];
            const float ex = x1-x0, ey = y1-y0;
            const float L = std::sqrt(ex*ex + ey*ey);
            if (L < 1e-4f) continue;
            const float ux = ex/L, uy = ey/L;
            const float nx = -uy, ny = ux;
            const float cx0 = x0 + nx*offset, cy0 = y0 + ny*offset;
            const float cx1 = x1 + nx*offset, cy1 = y1 + ny*offset;
            pushQuadLocal(out.rails,
                cx0 + nx*kRailHalfWidth, cy0 + ny*kRailHalfWidth,
                cx1 + nx*kRailHalfWidth, cy1 + ny*kRailHalfWidth,
                cx1 - nx*kRailHalfWidth, cy1 - ny*kRailHalfWidth,
                cx0 - nx*kRailHalfWidth, cy0 - ny*kRailHalfWidth,
                railR, railG, railB, railA);
        }
    };
    for (std::size_t i = 0; i < ng.lane_count(); ++i) {
        if (ng.lane_kind[i] != 1) continue;
        const std::uint32_t s = ng.lane_offsets[i];
        const std::uint32_t e = ng.lane_offsets[i + 1];
        const std::size_t nPts = e - s;
        if (nPts < 2) continue;
        const float* pts = &ng.lane_points[s * 2];
        pushRail(pts, nPts,  halfGauge);
        pushRail(pts, nPts, -halfGauge);
        pushSleepers(pts, nPts);
    }
    return out;
}

// ---------------------------------------------------------------------------
PolygonVerts buildPolygonVerts(const NetworkGeometry& ng) {
    PolygonVerts out;
    constexpr float kOutlineHalfWidth = 0.35f;  // world-space replacement for glLineWidth
    for (std::size_t i = 0; i < ng.polygon_count(); ++i) {
        const std::uint32_t s = ng.polygon_offsets[i];
        const std::uint32_t e = ng.polygon_offsets[i + 1];
        if (e - s < 2) continue;
        const quint8 r = ng.polygon_rgba[i * 4 + 0];
        const quint8 g = ng.polygon_rgba[i * 4 + 1];
        const quint8 b = ng.polygon_rgba[i * 4 + 2];
        const quint8 a = ng.polygon_rgba[i * 4 + 3];

        if (ng.polygon_filled[i] && e - s >= 3) {
            // Triangle fan around centroid → expand to a triangle list.
            float cx = 0.0f, cy = 0.0f;
            for (std::uint32_t p = s; p < e; ++p) {
                cx += ng.polygon_points[p * 2];
                cy += ng.polygon_points[p * 2 + 1];
            }
            cx /= static_cast<float>(e - s);
            cy /= static_cast<float>(e - s);
            const std::uint32_t n = e - s;
            for (std::uint32_t k = 0; k < n; ++k) {
                const std::uint32_t pa = s + k;
                const std::uint32_t pb = s + ((k + 1) % n);
                out.fills.push_back(V(cx, cy, r, g, b, a));
                out.fills.push_back(V(ng.polygon_points[pa * 2],
                                      ng.polygon_points[pa * 2 + 1],
                                      r, g, b, a));
                out.fills.push_back(V(ng.polygon_points[pb * 2],
                                      ng.polygon_points[pb * 2 + 1],
                                      r, g, b, a));
            }
        } else {
            // Outline: extrude as world-space strip; ~0.35m half-width.
            const std::size_t nPts = e - s;
            for (std::size_t k = 0; k + 1 < nPts; ++k) {
                pushSegmentQuad(out.outlines,
                    ng.polygon_points[(s + k) * 2],
                    ng.polygon_points[(s + k) * 2 + 1],
                    ng.polygon_points[(s + k + 1) * 2],
                    ng.polygon_points[(s + k + 1) * 2 + 1],
                    kOutlineHalfWidth, r, g, b, a);
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
NetworkVerts buildNetworkVerts(const NetworkGeometry& ng) {
    NetworkVerts out;
    const quint8 laneR=77,  laneG=77,  laneB=82,  laneA=255;
    const quint8 juncR=51,  juncG=51,  juncB=56,  juncA=255;

    // ---- Lane strip (TriangleStrip topology with degenerate bridges).
    out.laneStrip.reserve(ng.lane_points.size() * 3);
    bool first = true;
    for (std::size_t i = 0; i < ng.lane_count(); ++i) {
        const std::uint32_t s = ng.lane_offsets[i];
        const std::uint32_t e = ng.lane_offsets[i + 1];
        const std::size_t nPts = e - s;
        if (nPts < 2) continue;
        const std::size_t startV = out.laneStrip.size();
        extrudePolylineStrip(&ng.lane_points[s * 2], nPts,
                             ng.lane_widths[i],
                             laneR, laneG, laneB, laneA,
                             out.laneStrip);
        if (out.laneStrip.size() <= startV) continue;
        if (!first) {
            // Insert degenerate bridge between previous strip end and this
            // strip's start.
            const TrisColorVertex prevLast  = out.laneStrip[startV - 1];
            const TrisColorVertex nextFirst = out.laneStrip[startV];
            out.laneStrip.insert(out.laneStrip.begin() + startV,
                                 {prevLast, nextFirst});
        }
        first = false;
    }

    // ---- Junctions: fans → triangle list.
    for (std::size_t i = 0; i < ng.junction_count(); ++i) {
        const std::uint32_t s = ng.junction_offsets[i];
        const std::uint32_t e = ng.junction_offsets[i + 1];
        if (e - s < 3) continue;
        float cx = 0.0f, cy = 0.0f;
        for (std::uint32_t p = s; p < e; ++p) {
            cx += ng.junction_points[p * 2];
            cy += ng.junction_points[p * 2 + 1];
        }
        cx /= static_cast<float>(e - s);
        cy /= static_cast<float>(e - s);
        const std::uint32_t n = e - s;
        for (std::uint32_t k = 0; k < n; ++k) {
            const std::uint32_t pa = s + k;
            const std::uint32_t pb = s + ((k + 1) % n);
            out.juncTris.push_back(V(cx, cy, juncR, juncG, juncB, juncA));
            out.juncTris.push_back(V(ng.junction_points[pa * 2],
                                     ng.junction_points[pa * 2 + 1],
                                     juncR, juncG, juncB, juncA));
            out.juncTris.push_back(V(ng.junction_points[pb * 2],
                                     ng.junction_points[pb * 2 + 1],
                                     juncR, juncG, juncB, juncA));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
EdgeColorVerts buildEdgeColorVerts(const NetworkGeometry& ng) {
    EdgeColorVerts out;
    out.laneFirst.assign(ng.lane_count(), 0);
    out.laneCount.assign(ng.lane_count(), 0);
    out.strip.reserve(ng.lane_points.size() * 3);

    bool first = true;
    for (std::size_t i = 0; i < ng.lane_count(); ++i) {
        const std::uint32_t s = ng.lane_offsets[i];
        const std::uint32_t e = ng.lane_offsets[i + 1];
        const std::size_t nPts = e - s;
        if (nPts < 2 || ng.lane_kind[i] == 1 /*rail*/) continue;

        const std::size_t bridgeStart = out.strip.size();
        // Build this lane's strip into a tmp first so we can record laneFirst
        // unambiguously (after bridge insertion).
        std::vector<TrisColorVertex> lane;
        extrudePolylineStrip(&ng.lane_points[s * 2], nPts,
                             ng.lane_widths[i], 0, 0, 0, 0, lane);
        if (lane.empty()) continue;

        if (!first) {
            const TrisColorVertex prevLast = out.strip.back();
            const TrisColorVertex nextFirst = lane.front();
            out.strip.push_back(prevLast);
            out.strip.push_back(nextFirst);
        }
        const std::uint32_t laneFirst = static_cast<std::uint32_t>(out.strip.size());
        out.strip.insert(out.strip.end(), lane.begin(), lane.end());
        out.laneFirst[i] = laneFirst;
        out.laneCount[i] = static_cast<std::uint32_t>(lane.size());
        (void)bridgeStart;
        first = false;
    }
    return out;
}

}  // namespace layer_builders
