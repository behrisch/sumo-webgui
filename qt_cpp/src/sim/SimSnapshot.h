#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <QtGlobal>  // qint64

// A per-step snapshot of dynamic simulation state, intended for read-only
// consumption by the GUI thread. Layout mirrors the deck.gl/Float32Array
// approach: parallel typed arrays, no per-vehicle heap objects.
//
// Double-buffering is owned by SimWorker; consumers receive a const shared
// pointer to a snapshot that won't change for the lifetime of that pointer.
struct SimSnapshot {
    double simTime  = 0.0;
    qint64 stepIdx  = 0;

    // Vehicles. All arrays have length vehicle_count = ids.size().
    std::vector<std::string> ids;
    std::vector<float>       x;       // world XY (meters)
    std::vector<float>       y;
    std::vector<float>       cos_a;   // pre-computed cos(angle)
    std::vector<float>       sin_a;   // pre-computed sin(angle)
    std::vector<std::uint8_t> rgba;   // r0,g0,b0,a0, r1,g1,b1,a1, ...

    // Persons. Same layout, parallel arrays.
    std::vector<std::string> person_ids;
    std::vector<float>       person_x;
    std::vector<float>       person_y;
    std::vector<std::uint8_t> person_rgba;

    // Traffic-light states, keyed by TLS id. value is the raw RYG string.
    std::unordered_map<std::string, std::string> tls_states;

    [[nodiscard]] std::size_t vehicle_count() const noexcept { return ids.size(); }
    [[nodiscard]] std::size_t person_count()  const noexcept { return person_ids.size(); }
};

using SimSnapshotPtr = std::shared_ptr<const SimSnapshot>;
