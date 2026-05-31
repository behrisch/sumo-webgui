// NetworkCache: read the shared `.pb` network geometry the ecal_deck
// Python publisher writes (`__sumocache__/<base>.net.vN.bin` next to the
// .net.xml). When present, lets the qt_cpp UI populate its
// `NetworkGeometry` *without* spinning up libsumo first — useful for the
// "show network instantly on file open" UX, before Simulation::start()
// finishes.
//
// Cache format v2: positions are stored as raw SUMO XY in float32. The qt
// renderer is XY-only (float32 vertex buffers), so we can read and use
// them directly with no projection step. The bridge handles the
// cartesian→lonlat conversion for the frontend separately.
//
// Scope: only the network family (lanes + junctions + TLS bars). The
// `__sumocache__/*.poly.v*.bin`, `*.stops.v*.bin`, `*.det.v*.bin` files
// hold polygons / POIs / stopping places / detectors and are NOT consumed
// here yet — those still come from the live libsumo extraction after
// start.
//
// Stay version-locked with `_CACHE_VERSION` in `sumo_ecal_publisher.py`
// (mismatched caches are silently rejected — caller falls back to
// libsumo).

#include "NetworkCache.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "sumo_ecal.pb.h"  // exposed by libsumocpp via FindLibsumo.cmake

#include "NetworkGeometry.h"

namespace fs = std::filesystem;

namespace {

// Cache layout version. MUST match _CACHE_VERSION in
// ecal_deck/sumo_ecal_publisher.py.  Bump in lockstep with any
// incompatible NetworkGeometry change on either side.
constexpr int kCacheVersion = 2;

// Read entire file as bytes. Returns empty on any error.
std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Unpack a `bytes` field that contains a packed little-endian array of T.
// Returns a copy; caller can iterate or std::move. Endianness assumption
// matches the Python `_array.array(...).tobytes()` writers — x86/ARM
// little-endian, same as Python and the C++ deserializer here. Adding a
// big-endian byteswap if we ever ship to a BE platform is left as TODO.
template <typename T>
std::vector<T> unpack(const std::string& blob) {
    std::vector<T> out;
    if (blob.empty() || (blob.size() % sizeof(T)) != 0) return out;
    out.resize(blob.size() / sizeof(T));
    std::memcpy(out.data(), blob.data(), blob.size());
    return out;
}

// Append `n` points starting at `src` to `dst_points` (qt expects float32
// XY pairs) and grow the running bbox. `src` is f32 X,Y,X,Y,… raw SUMO XY
// straight from the cache — no projection needed in cache v2+.
void copyPointsToFloat(const float* src, std::size_t nPoints,
                       std::vector<float>& dst,
                       float& minX, float& minY,
                       float& maxX, float& maxY) {
    // NOTE: callers MUST pre-reserve `dst` to the final total. Calling
    // reserve() per-lane was O(n²) on Berlin (728k lanes → 728k reallocs
    // of the growing buffer, each copying everything previously written).
    for (std::size_t i = 0; i < nPoints; ++i) {
        const float x = src[i * 2];
        const float y = src[i * 2 + 1];
        dst.push_back(x);
        dst.push_back(y);
        minX = std::min(minX, x); minY = std::min(minY, y);
        maxX = std::max(maxX, x); maxY = std::max(maxY, y);
    }
}

// Translate (lane_function, lane_perm_class) — see sumo_ecal.proto field
// docs — to qt's NetworkGeometry::lane_kind taxonomy.
std::uint8_t mapLaneKind(std::uint8_t func, std::uint8_t perm) {
    // crossings / walking areas → walkingarea
    if (func == 2 || func == 3) return 3;
    // internal junction connector
    if (func == 1) return 4;
    // rail-only permission
    if (perm == 3) return 1;
    // pedestrian / other permission
    if (perm == 0) return 2;
    // motorised / bike / embedded rail → treat as road
    return 0;
}

}  // namespace

namespace network_cache {

std::string resolveNetFile(const std::string& sumocfgPath) {
    if (sumocfgPath.empty()) return {};
    const std::string xml = slurp(sumocfgPath);
    if (xml.empty()) return {};
    // Match <net-file value="..."/> (or single quotes). The sumocfg schema
    // allows whitespace and other attributes; we only need the `value`.
    // Conservative regex: any whitespace separator, capture inside the
    // first matching quoted value attribute that follows <net-file…>.
    static const std::regex kNetFileRe(
        R"(<\s*net-file\b[^>]*\bvalue\s*=\s*["']([^"']+)["'])",
        std::regex::ECMAScript | std::regex::icase);
    std::smatch m;
    if (!std::regex_search(xml, m, kNetFileRe)) return {};
    fs::path rel(m[1].str());
    if (rel.is_absolute()) return rel.string();
    fs::path cfgDir = fs::path(sumocfgPath).parent_path();
    return (cfgDir / rel).lexically_normal().string();
}

std::string cachePathFor(const std::string& netFile) {
    if (netFile.empty()) return {};
    fs::path p(netFile);
    const fs::path dir = p.parent_path() / "__sumocache__";
    const std::string base = p.filename().string();
    std::ostringstream name;
    name << base << ".net.v" << kCacheVersion << ".bin";
    return (dir / name.str()).string();
}

std::shared_ptr<NetworkGeometry> tryLoadCache(const std::string& sumocfgPath) {
    std::fprintf(stderr, "[NetworkCache] tryLoadCache(%s) — start\n", sumocfgPath.c_str());
    const std::string netFile = resolveNetFile(sumocfgPath);
    if (netFile.empty()) { std::fprintf(stderr, "[NetworkCache] no net-file in sumocfg\n"); return nullptr; }
    const std::string cacheFile = cachePathFor(netFile);
    if (cacheFile.empty()) return nullptr;
    std::fprintf(stderr, "[NetworkCache] candidate cache: %s\n", cacheFile.c_str());

    std::error_code ec;
    if (!fs::exists(cacheFile, ec) || ec) { std::fprintf(stderr, "[NetworkCache] cache file missing\n"); return nullptr; }
    if (!fs::exists(netFile, ec) || ec)   { std::fprintf(stderr, "[NetworkCache] net file missing\n"); return nullptr; }

    // Stale-cache check: net file rebuilt since cache was written.
    const auto cacheMt = fs::last_write_time(cacheFile, ec);
    if (ec) return nullptr;
    const auto netMt   = fs::last_write_time(netFile, ec);
    if (ec) return nullptr;
    if (cacheMt < netMt) { std::fprintf(stderr, "[NetworkCache] cache older than net file — stale\n"); return nullptr; }

    std::fprintf(stderr, "[NetworkCache] slurping cache (%lld bytes on disk)\n",
                 (long long)fs::file_size(cacheFile, ec));
    const std::string bytes = slurp(cacheFile);
    if (bytes.empty()) { std::fprintf(stderr, "[NetworkCache] slurp returned empty\n"); return nullptr; }
    std::fprintf(stderr, "[NetworkCache] slurped %zu bytes; parsing\n", bytes.size());

    sumo::NetworkGeometry pb;
    if (!pb.ParseFromString(bytes)) { std::fprintf(stderr, "[NetworkCache] ParseFromString failed\n"); return nullptr; }
    std::fprintf(stderr, "[NetworkCache] parsed OK: version=%u geo=%d lanes=%d junc=%d tls=%d\n",
                 pb.version(), (int)pb.geo_referenced(),
                 pb.lane_ids_size(), pb.junction_ids_size(), pb.tls_entries_size());
    if (pb.version() != static_cast<std::uint32_t>(kCacheVersion)) {
        std::fprintf(stderr,
            "[NetworkCache] %s: version mismatch (cache=%u, expected=%d) — falling back to libsumo\n",
            cacheFile.c_str(), pb.version(), kCacheVersion);
        return nullptr;
    }

    // Cache v2: positions are raw SUMO XY in float32. No projection
    // needed — the qt renderer is XY-only and the bridge handles the
    // lonlat conversion for the frontend.

    auto ng = std::make_shared<NetworkGeometry>();

    float minX = std::numeric_limits<float>::infinity();
    float minY = std::numeric_limits<float>::infinity();
    float maxX = -std::numeric_limits<float>::infinity();
    float maxY = -std::numeric_limits<float>::infinity();

    // ---- Lanes ----
    const auto        lanePos    = unpack<float>(pb.lane_positions());
    const auto        laneStarts = unpack<std::uint32_t>(pb.lane_starts());
    const auto        laneW      = unpack<float>(pb.lane_widths());
    const auto        laneFunc   = unpack<std::uint8_t>(pb.lane_function());
    const auto        lanePerm   = unpack<std::uint8_t>(pb.lane_perm_class());
    const std::size_t nLanes     = static_cast<std::size_t>(pb.lane_ids_size());

    std::fprintf(stderr, "[NetworkCache] lanes: unpacking buffers (nLanes=%zu, lane_positions=%zu bytes)\n",
                 nLanes, (std::size_t)pb.lane_positions().size());

    // lane_starts has a sentinel: len == nLanes + 1.
    if (laneStarts.size() != nLanes + 1) return nullptr;
    if (laneW.size()      != nLanes)     return nullptr;

    std::fprintf(stderr, "[NetworkCache] lanes: filling NetworkGeometry\n");

    ng->lane_ids.reserve(nLanes);
    ng->lane_widths.reserve(nLanes);
    ng->lane_offsets.reserve(nLanes + 1);
    ng->lane_kind.reserve(nLanes);
    ng->lane_points.reserve(lanePos.size());  // exact total — avoid O(n²) regrow

    for (std::size_t i = 0; i < nLanes; ++i) {
        ng->lane_ids.push_back(pb.lane_ids(static_cast<int>(i)));
        ng->lane_widths.push_back(laneW[i]);
        ng->lane_offsets.push_back(
            static_cast<std::uint32_t>(ng->lane_points.size() / 2));
        const std::uint32_t s = laneStarts[i];
        const std::uint32_t e = laneStarts[i + 1];
        if (e < s) return nullptr;
        if (static_cast<std::size_t>(e) * 2 > lanePos.size()) return nullptr;
        copyPointsToFloat(lanePos.data() + std::size_t(s) * 2, e - s,
                          ng->lane_points, minX, minY, maxX, maxY);

        const std::uint8_t f = i < laneFunc.size() ? laneFunc[i] : 0;
        const std::uint8_t p = i < lanePerm.size() ? lanePerm[i] : 2;
        ng->lane_kind.push_back(mapLaneKind(f, p));
    }
    ng->lane_offsets.push_back(
        static_cast<std::uint32_t>(ng->lane_points.size() / 2));
    std::fprintf(stderr, "[NetworkCache] lanes: done (lane_points=%zu)\n", ng->lane_points.size() / 2);

    // ---- Junctions ----
    const auto        juncPos    = unpack<float>(pb.junction_positions());
    const auto        juncStarts = unpack<std::uint32_t>(pb.junction_starts());
    const std::size_t nJunc      = static_cast<std::size_t>(pb.junction_ids_size());
    if (juncStarts.size() != nJunc + 1) return nullptr;

    ng->junction_ids.reserve(nJunc);
    ng->junction_offsets.reserve(nJunc + 1);
    ng->junction_points.reserve(juncPos.size());  // exact total — avoid O(n²) regrow
    for (std::size_t i = 0; i < nJunc; ++i) {
        ng->junction_ids.push_back(pb.junction_ids(static_cast<int>(i)));
        ng->junction_offsets.push_back(
            static_cast<std::uint32_t>(ng->junction_points.size() / 2));
        const std::uint32_t s = juncStarts[i];
        const std::uint32_t e = juncStarts[i + 1];
        if (e < s) return nullptr;
        if (static_cast<std::size_t>(e) * 2 > juncPos.size()) return nullptr;
        copyPointsToFloat(juncPos.data() + std::size_t(s) * 2, e - s,
                          ng->junction_points, minX, minY, maxX, maxY);
    }
    ng->junction_offsets.push_back(
        static_cast<std::uint32_t>(ng->junction_points.size() / 2));

    // ---- TLS bars ----
    // tls_positions: 4 floats per bar (x1,y1,x2,y2).  tls_entries:
    // one entry per bar, parallel.  Convert (x1,y1)->(x2,y2) into the
    // qt format (center + lane forward tangent + width).
    const auto        tlsPos = unpack<float>(pb.tls_positions());
    const std::size_t nTls   = static_cast<std::size_t>(pb.tls_entries_size());
    if (tlsPos.size() == nTls * 4) {
        ng->tls_ids.reserve(nTls);
        ng->tls_state_index.reserve(nTls);
        ng->tls_x.reserve(nTls);
        ng->tls_y.reserve(nTls);
        ng->tls_dx.reserve(nTls);
        ng->tls_dy.reserve(nTls);
        ng->tls_w.reserve(nTls);
        for (std::size_t i = 0; i < nTls; ++i) {
            const float x1 = tlsPos[i * 4 + 0];
            const float y1 = tlsPos[i * 4 + 1];
            const float x2 = tlsPos[i * 4 + 2];
            const float y2 = tlsPos[i * 4 + 3];
            const float bx = x2 - x1, by = y2 - y1;
            const float w  = std::sqrt(bx * bx + by * by);
            const float cx = 0.5f * (x1 + x2);
            const float cy = 0.5f * (y1 + y2);
            // Lane forward tangent = perpendicular to bar, unit length.
            // The sign is irrelevant — bar is drawn symmetrically.
            float dx = 1.0f, dy = 0.0f;
            if (w > 1e-9f) {
                dx = -by / w;
                dy =  bx / w;
            }
            ng->tls_x.push_back(cx);
            ng->tls_y.push_back(cy);
            ng->tls_dx.push_back(dx);
            ng->tls_dy.push_back(dy);
            ng->tls_w.push_back(w);

            const auto& te = pb.tls_entries(static_cast<int>(i));
            ng->tls_ids.push_back(te.tls());
            ng->tls_state_index.push_back(static_cast<std::uint32_t>(te.tl_index()));
            // Note: bbox already covered by lane/junction points.
            minX = std::min(minX, cx); minY = std::min(minY, cy);
            maxX = std::max(maxX, cx); maxY = std::max(maxY, cy);
        }
    }

    // Polygons / POIs / stopping places / detectors come from separate
    // cache families (poly.vN.bin / stops.vN.bin / det.vN.bin) and are
    // out of scope here — see file-level comment.

    if (!std::isfinite(minX)) {
        minX = minY = -100.0f;
        maxX = maxY =  100.0f;
    }
    ng->min_x = minX;
    ng->min_y = minY;
    ng->max_x = maxX;
    ng->max_y = maxY;
    std::fprintf(stderr,
        "[NetworkCache] HIT %s — %zu lanes, %zu junctions, %zu TLS bars, "
        "bbox=(%.1f,%.1f)-(%.1f,%.1f)\n",
        cacheFile.c_str(), ng->lane_count(), ng->junction_count(),
        ng->tls_marker_count(), ng->min_x, ng->min_y, ng->max_x, ng->max_y);
    return ng;
}

}  // namespace network_cache
