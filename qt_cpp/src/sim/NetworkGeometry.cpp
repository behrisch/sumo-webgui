#include "NetworkGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <libsumo/Junction.h>
#include <libsumo/Lane.h>
#include <libsumo/Simulation.h>
#include <libsumo/TraCIDefs.h>

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
        for (const auto& id : laneIds) {
            ng->lane_ids.push_back(id);
            ng->lane_widths.push_back(static_cast<float>(libsumo::Lane::getWidth(id)));
            appendShape(libsumo::Lane::getShape(id),
                        ng->lane_points, ng->lane_offsets,
                        minX, minY, maxX, maxY);
        }
        ng->lane_offsets.push_back(static_cast<std::uint32_t>(ng->lane_points.size() / 2));
    }

    // Junctions
    {
        const auto juncIds = libsumo::Junction::getIDList();
        ng->junction_ids.reserve(juncIds.size());
        ng->junction_offsets.reserve(juncIds.size() + 1);
        for (const auto& id : juncIds) {
            ng->junction_ids.push_back(id);
            appendShape(libsumo::Junction::getShape(id),
                        ng->junction_points, ng->junction_offsets,
                        minX, minY, maxX, maxY);
        }
        ng->junction_offsets.push_back(
            static_cast<std::uint32_t>(ng->junction_points.size() / 2));
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
