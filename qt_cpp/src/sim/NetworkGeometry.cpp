#include "NetworkGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#pragma push_macro("signals")
#undef signals
#include <libsumo/BusStop.h>
#include <libsumo/ChargingStation.h>
#include <libsumo/InductionLoop.h>
#include <libsumo/Junction.h>
#include <libsumo/Lane.h>
#include <libsumo/LaneArea.h>
#include <libsumo/MultiEntryExit.h>
#include <libsumo/POI.h>
#include <libsumo/ParkingArea.h>
#include <libsumo/Polygon.h>
#include <libsumo/Simulation.h>
#include <libsumo/TraCIDefs.h>
#include <libsumo/TrafficLight.h>
#pragma pop_macro("signals")

namespace {

void appendShape(const libsumo::TraCIPositionVector& shape,
                 std::vector<float>& points,
                 std::vector<std::uint32_t>& offsets,
                 float& minX, float& minY, float& maxX, float& maxY) {
    offsets.push_back(static_cast<std::uint32_t>(points.size() / 2));
    for (const auto& p : shape.value) {
        const float x = static_cast<float>(p.x);
        const float y = static_cast<float>(p.y);
        points.push_back(x);
        points.push_back(y);
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }
}

}  // namespace

std::shared_ptr<NetworkGeometry> buildNetworkGeometry() {
    auto ng = std::make_shared<NetworkGeometry>();

    float minX = std::numeric_limits<float>::infinity();
    float minY = std::numeric_limits<float>::infinity();
    float maxX = -std::numeric_limits<float>::infinity();
    float maxY = -std::numeric_limits<float>::infinity();

    // Lanes (includes internal lanes when libsumo is run normally; we keep
    // all of them — the deck.gl frontend does the same and uses lane.function
    // to colour them differently. Phase 1: render uniformly.)
    {
        const auto laneIds = libsumo::Lane::getIDList();
        ng->lane_ids.reserve(laneIds.size());
        ng->lane_offsets.reserve(laneIds.size() + 1);
        ng->lane_widths.reserve(laneIds.size());
        ng->lane_kind.reserve(laneIds.size());
        for (const auto& id : laneIds) {
            ng->lane_ids.push_back(id);
            ng->lane_widths.push_back(static_cast<float>(libsumo::Lane::getWidth(id)));
            appendShape(libsumo::Lane::getShape(id),
                        ng->lane_points, ng->lane_offsets,
                        minX, minY, maxX, maxY);

            // Classify by allowed vehicle classes.
            std::uint8_t kind = 0;
            const bool internal = !id.empty() && id.front() == ':';
            try {
                const auto allowed = libsumo::Lane::getAllowed(id);
                bool hasRail = false, hasPed = false, hasRoad = false;
                for (const auto& vc : allowed) {
                    if (vc.find("rail") != std::string::npos
                        || vc == "tram" || vc == "cable_car"
                        || vc == "subway" || vc == "light_rail") hasRail = true;
                    else if (vc == "pedestrian") hasPed = true;
                    else hasRoad = true;
                }
                if (allowed.empty()) hasRoad = true;
                if      (hasRail && !hasRoad) kind = 1;
                else if (hasPed  && !hasRoad && !hasRail) kind = internal ? 3 : 2;
                else if (internal)                         kind = 4;
                else                                       kind = 0;
            } catch (...) {
                kind = internal ? 4 : 0;
            }
            ng->lane_kind.push_back(kind);
        }
        ng->lane_offsets.push_back(static_cast<std::uint32_t>(ng->lane_points.size() / 2));
    }

    // Junctions
    std::unordered_map<std::string, std::pair<float, float>> juncCenters;
    {
        const auto juncIds = libsumo::Junction::getIDList();
        ng->junction_ids.reserve(juncIds.size());
        ng->junction_offsets.reserve(juncIds.size() + 1);
        for (const auto& id : juncIds) {
            ng->junction_ids.push_back(id);
            const std::size_t before = ng->junction_points.size() / 2;
            appendShape(libsumo::Junction::getShape(id),
                        ng->junction_points, ng->junction_offsets,
                        minX, minY, maxX, maxY);
            const std::size_t after = ng->junction_points.size() / 2;
            float cx = 0.0f, cy = 0.0f;
            if (after > before) {
                for (std::size_t p = before; p < after; ++p) {
                    cx += ng->junction_points[p * 2];
                    cy += ng->junction_points[p * 2 + 1];
                }
                const float inv = 1.0f / static_cast<float>(after - before);
                cx *= inv; cy *= inv;
            }
            juncCenters.emplace(id, std::make_pair(cx, cy));
        }
        ng->junction_offsets.push_back(
            static_cast<std::uint32_t>(ng->junction_points.size() / 2));
    }

    // Polygons. Triangulation is done later in PolygonLayer; here we just
    // record raw shape, color, and filled flag.
    try {
        const auto polyIds = libsumo::Polygon::getIDList();
        ng->polygon_offsets.reserve(polyIds.size() + 1);
        for (const auto& id : polyIds) {
            const auto shape = libsumo::Polygon::getShape(id);
            appendShape(shape, ng->polygon_points, ng->polygon_offsets,
                        minX, minY, maxX, maxY);
            const auto c = libsumo::Polygon::getColor(id);
            ng->polygon_rgba.push_back(static_cast<std::uint8_t>(c.r));
            ng->polygon_rgba.push_back(static_cast<std::uint8_t>(c.g));
            ng->polygon_rgba.push_back(static_cast<std::uint8_t>(c.b));
            ng->polygon_rgba.push_back(static_cast<std::uint8_t>(c.a));
            ng->polygon_filled.push_back(libsumo::Polygon::getFilled(id) ? 1 : 0);
        }
        ng->polygon_offsets.push_back(
            static_cast<std::uint32_t>(ng->polygon_points.size() / 2));
    } catch (...) {
        // No polygons loaded; harmless.
    }

    // POIs (Points of Interest).
    try {
        const auto poiIds = libsumo::POI::getIDList();
        for (const auto& id : poiIds) {
            libsumo::TraCIPosition p;
            try { p = libsumo::POI::getPosition(id); }
            catch (...) { continue; }
            libsumo::TraCIColor c(220, 220, 60, 255);
            try { c = libsumo::POI::getColor(id); } catch (...) {}
            std::string ty;
            try { ty = libsumo::POI::getType(id); } catch (...) {}
            ng->poi_x.push_back(static_cast<float>(p.x));
            ng->poi_y.push_back(static_cast<float>(p.y));
            ng->poi_rgba.push_back(static_cast<std::uint8_t>(c.r));
            ng->poi_rgba.push_back(static_cast<std::uint8_t>(c.g));
            ng->poi_rgba.push_back(static_cast<std::uint8_t>(c.b));
            ng->poi_rgba.push_back(static_cast<std::uint8_t>(c.a));
            ng->poi_ids.push_back(id);
            ng->poi_types.push_back(std::move(ty));
            const float x = static_cast<float>(p.x);
            const float y = static_cast<float>(p.y);
            minX = std::min(minX, x); minY = std::min(minY, y);
            maxX = std::max(maxX, x); maxY = std::max(maxY, y);
        }
    } catch (...) {
        // No POIs loaded; harmless.
    }

    // Traffic light heads. Place one marker per controlled link at the end
    // of its fromLane (closest to the stop line). Fallback to junction
    // center if the lane isn't found.
    try {
        // Build a fromLane -> (endpoint, tangent, width, laneIdx) map from
        // the already-extracted lane shapes.
        struct LaneEnd { float x, y, dx, dy, w; std::size_t idx; };
        std::unordered_map<std::string, LaneEnd> laneEnds;
        laneEnds.reserve(ng->lane_count());
        for (std::size_t i = 0; i < ng->lane_count(); ++i) {
            const std::uint32_t s = ng->lane_offsets[i];
            const std::uint32_t e = ng->lane_offsets[i + 1];
            if (e - s < 1) continue;
            const std::uint32_t last = e - 1;
            const float lx = ng->lane_points[last * 2];
            const float ly = ng->lane_points[last * 2 + 1];
            float dx = 1.0f, dy = 0.0f;
            if (e - s >= 2) {
                const std::uint32_t prev = e - 2;
                dx = lx - ng->lane_points[prev * 2];
                dy = ly - ng->lane_points[prev * 2 + 1];
                const float n = std::sqrt(dx * dx + dy * dy);
                if (n > 1e-6f) { dx /= n; dy /= n; } else { dx = 1.0f; dy = 0.0f; }
            }
            laneEnds.emplace(ng->lane_ids[i],
                LaneEnd{lx, ly, dx, dy, ng->lane_widths[i], i});
        }

        const auto tlsIds = libsumo::TrafficLight::getIDList();
        for (const auto& tid : tlsIds) {
            std::vector<std::vector<libsumo::TraCILink>> links;
            try { links = libsumo::TrafficLight::getControlledLinks(tid); }
            catch (...) { continue; }
            for (std::size_t i = 0; i < links.size(); ++i) {
                float x = 0.0f, y = 0.0f;
                bool have = false;
                const LaneEnd* le = nullptr;
                if (!links[i].empty()) {
                    const auto& lk = links[i].front();
                    auto it = laneEnds.find(lk.fromLane);
                    if (it != laneEnds.end()) {
                        x = it->second.x;
                        y = it->second.y;
                        le = &it->second;
                        have = true;
                    }
                }
                if (!have) {
                    auto jit = juncCenters.find(tid);
                    if (jit != juncCenters.end()) {
                        x = jit->second.first;
                        y = jit->second.second;
                    }
                }
                ng->tls_x.push_back(x);
                ng->tls_y.push_back(y);
                if (le) {
                    ng->tls_dx.push_back(le->dx);
                    ng->tls_dy.push_back(le->dy);
                    ng->tls_w.push_back(le->w);
                } else {
                    ng->tls_dx.push_back(1.0f);
                    ng->tls_dy.push_back(0.0f);
                    ng->tls_w.push_back(3.2f);  // sensible default lane width
                }
                ng->tls_ids.push_back(tid);
                ng->tls_state_index.push_back(static_cast<std::uint32_t>(i));
                // NOTE: we used to push a stopline bar here as well — same
                // position, same width — which made every TLS signal render
                // as two overlapping bars (a coloured TLS bar on top of a
                // white stop-line bar).  The TLS bar already serves that
                // purpose; real stop-lines for *uncontrolled* minor
                // approaches are not yet emitted by this builder (parity
                // gap vs ecal_deck's `lane_has_stopline`).
            }
        }
    } catch (...) {
        // No TLS; harmless.
    }

    // ---- Stopping places + detectors: longitudinal position on a lane ----
    // Build a per-lane offset map first.
    std::unordered_map<std::string, std::size_t> laneIdx;
    laneIdx.reserve(ng->lane_count());
    for (std::size_t i = 0; i < ng->lane_count(); ++i) {
        laneIdx.emplace(ng->lane_ids[i], i);
    }

    auto laneInterp = [&](const std::string& laneId, double s,
                          float& x, float& y, float& dx, float& dy,
                          float& width) -> bool {
        auto it = laneIdx.find(laneId);
        if (it == laneIdx.end()) return false;
        const std::size_t li = it->second;
        const std::uint32_t a = ng->lane_offsets[li];
        const std::uint32_t b = ng->lane_offsets[li + 1];
        if (b - a < 2) return false;
        width = ng->lane_widths[li];
        // Walk segments until cumulative length >= s.
        double accum = 0.0;
        for (std::uint32_t k = a; k + 1 < b; ++k) {
            const float x0 = ng->lane_points[k * 2];
            const float y0 = ng->lane_points[k * 2 + 1];
            const float x1 = ng->lane_points[(k + 1) * 2];
            const float y1 = ng->lane_points[(k + 1) * 2 + 1];
            const double ex = x1 - x0, ey = y1 - y0;
            const double slen = std::sqrt(ex * ex + ey * ey);
            if (s <= accum + slen || k + 2 == b) {
                const double t = slen > 1e-6 ? (s - accum) / slen : 0.0;
                const double tc = std::clamp(t, 0.0, 1.0);
                x = static_cast<float>(x0 + tc * ex);
                y = static_cast<float>(y0 + tc * ey);
                const double n = slen > 1e-6 ? slen : 1.0;
                dx = static_cast<float>(ex / n);
                dy = static_cast<float>(ey / n);
                return true;
            }
            accum += slen;
        }
        return false;
    };

    auto addStop = [&](std::uint8_t kind, const std::string& id,
                       const std::string& lane, double s0, double s1) {
        const double mid = 0.5 * (s0 + s1);
        const double len = std::max(0.5, s1 - s0);
        float x, y, dx, dy, w;
        if (!laneInterp(lane, mid, x, y, dx, dy, w)) return;
        ng->stop_x.push_back(x);
        ng->stop_y.push_back(y);
        ng->stop_dx.push_back(dx);
        ng->stop_dy.push_back(dy);
        ng->stop_len.push_back(static_cast<float>(len));
        ng->stop_w.push_back(w);
        ng->stop_kind.push_back(kind);
        ng->stop_ids.push_back(id);
    };

    auto enumerateStops = [&](std::uint8_t kind, auto idListFn,
                              auto laneFn, auto startFn, auto endFn) {
        try {
            for (const auto& id : idListFn()) {
                try {
                    addStop(kind, id, laneFn(id), startFn(id), endFn(id));
                } catch (...) {}
            }
        } catch (...) {}
    };
    enumerateStops(0, libsumo::BusStop::getIDList,
                   libsumo::BusStop::getLaneID,
                   libsumo::BusStop::getStartPos,
                   libsumo::BusStop::getEndPos);
    enumerateStops(1, libsumo::ChargingStation::getIDList,
                   libsumo::ChargingStation::getLaneID,
                   libsumo::ChargingStation::getStartPos,
                   libsumo::ChargingStation::getEndPos);
    enumerateStops(2, libsumo::ParkingArea::getIDList,
                   libsumo::ParkingArea::getLaneID,
                   libsumo::ParkingArea::getStartPos,
                   libsumo::ParkingArea::getEndPos);

    auto addDet = [&](std::uint8_t kind, const std::string& id,
                      const std::string& lane, double s, double len) {
        float x, y, dx, dy, w;
        if (!laneInterp(lane, s + 0.5 * len, x, y, dx, dy, w)) return;
        (void)w;
        ng->det_x.push_back(x);
        ng->det_y.push_back(y);
        ng->det_dx.push_back(dx);
        ng->det_dy.push_back(dy);
        ng->det_len.push_back(static_cast<float>(len));
        ng->det_kind.push_back(kind);
        ng->det_ids.push_back(id);
    };
    try {
        for (const auto& id : libsumo::InductionLoop::getIDList()) {
            try {
                addDet(0, id, libsumo::InductionLoop::getLaneID(id),
                       libsumo::InductionLoop::getPosition(id), 0.0);
            } catch (...) {}
        }
    } catch (...) {}
    try {
        for (const auto& id : libsumo::LaneArea::getIDList()) {
            try {
                addDet(1, id, libsumo::LaneArea::getLaneID(id),
                       libsumo::LaneArea::getPosition(id),
                       libsumo::LaneArea::getLength(id));
            } catch (...) {}
        }
    } catch (...) {}
    try {
        for (const auto& id : libsumo::MultiEntryExit::getIDList()) {
            try {
                const auto eLanes = libsumo::MultiEntryExit::getEntryLanes(id);
                const auto ePos   = libsumo::MultiEntryExit::getEntryPositions(id);
                for (std::size_t i = 0; i < eLanes.size() && i < ePos.size(); ++i) {
                    addDet(2, id + ":in" + std::to_string(i),
                           eLanes[i], ePos[i], 0.0);
                }
                const auto xLanes = libsumo::MultiEntryExit::getExitLanes(id);
                const auto xPos   = libsumo::MultiEntryExit::getExitPositions(id);
                for (std::size_t i = 0; i < xLanes.size() && i < xPos.size(); ++i) {
                    addDet(3, id + ":out" + std::to_string(i),
                           xLanes[i], xPos[i], 0.0);
                }
            } catch (...) {}
        }
    } catch (...) {}

    if (!std::isfinite(minX)) {
        // Empty network — fall back to net boundary.
        try {
            const auto b = libsumo::Simulation::getNetBoundary();
            if (b.value.size() >= 2) {
                minX = static_cast<float>(b.value[0].x);
                minY = static_cast<float>(b.value[0].y);
                maxX = static_cast<float>(b.value[1].x);
                maxY = static_cast<float>(b.value[1].y);
            } else {
                minX = minY = -100.0f;
                maxX = maxY = 100.0f;
            }
        } catch (...) {
            minX = minY = -100.0f;
            maxX = maxY = 100.0f;
        }
    }

    ng->min_x = minX;
    ng->min_y = minY;
    ng->max_x = maxX;
    ng->max_y = maxY;
    return ng;
}
