#include "NetworkGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#pragma push_macro("signals")
#undef signals
#include <libsumo/Junction.h>
#include <libsumo/Lane.h>
#include <libsumo/POI.h>
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
                ng->tls_ids.push_back(tid);
                ng->tls_state_index.push_back(static_cast<std::uint32_t>(i));

                if (le) {
                    ng->stopline_x.push_back(le->x);
                    ng->stopline_y.push_back(le->y);
                    ng->stopline_dx.push_back(le->dx);
                    ng->stopline_dy.push_back(le->dy);
                    ng->stopline_w.push_back(le->w);
                }
            }
        }
    } catch (...) {
        // No TLS; harmless.
    }

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
