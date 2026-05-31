// NetworkCache: read the shared `.pb` network geometry the ecal_deck
// Python publisher writes (`__ecaldeck__/<base>.net.vN.bin` next to the
// .net.xml). When present, lets the qt_cpp UI populate its
// `NetworkGeometry` *without* spinning up libsumo first — useful for the
// "show network instantly on file open" UX, before Simulation::start()
// finishes.
//
// Geo-referenced caches: the publisher stores lon/lat for the deck.gl
// frontend. We invert that here using PROJ + the cached proj_parameter
// + net_offset so the qt renderer (SUMO-XY-only) gets back what it
// expects. See `GeoInverter` for the math (mirror of
// `GeoConvHelper::cartesian2geo`).
//
// Scope: only the network family (lanes + junctions + TLS bars). The
// `__ecaldeck__/*.poly.v*.bin`, `*.stops.v*.bin`, `*.det.v*.bin` files
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

#include <proj.h>

#include "sumo_ecal.pb.h"  // exposed by libsumocpp via FindLibsumo.cmake

#include "NetworkGeometry.h"

namespace fs = std::filesystem;

namespace {

// Cache layout version. MUST match _CACHE_VERSION in
// ecal_deck/sumo_ecal_publisher.py.  Bump in lockstep with any
// incompatible NetworkGeometry change on either side.
constexpr int kCacheVersion = 1;

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

// Inverts the publisher's XY→lon/lat conversion using the projParameter
// + netOffset stored in the cache. Mirrors `GeoConvHelper::cartesian2geo`:
//
//   cartesian2geo(c):  c = c - offsetBase;  c = proj_inverse(c);
//
// so the inverse direction (lon/lat → SUMO XY) is:
//
//   xy = proj_forward(lonlat) + offsetBase
//
// (offsetBase is the value of <location netOffset="..."/>, which in the
// stored "-x,-y" format makes back-projected meters negative — adding it
// yields the small SUMO XY range the renderer expects.)
//
// Designed for bulk operation via `transformInPlace` — see
// `proj_trans_generic` docs. PROJ's per-call overhead dominates the
// trivial UTM math, so transforming all lane/junction/TLS points in 3
// batched calls is roughly an order of magnitude faster than calling
// `proj_trans` per point.
class GeoInverter {
public:
    GeoInverter(const std::string& proj4, double ox, double oy)
        : m_ox(ox), m_oy(oy) {
        m_ctx = proj_context_create();
        if (!m_ctx) return;
        // Disable PROJ's network grid fetching.  Without this, transforms
        // that need a datum-shift grid (e.g. ETRS89 ↔ WGS84 for European
        // nets like Berlin) will try to download from cdn.proj.org and
        // block the worker thread indefinitely if the machine is offline
        // or the CDN is unreachable.  Visualization only needs ~meter
        // accuracy, so the no-grid fallback PROJ chooses automatically
        // (a ballpark Helmert transform or pure ellipsoid math) is fine.
        proj_context_set_enable_network(m_ctx, 0);
        // Source: lon/lat in degrees (EPSG:4326-equivalent +proj=longlat
        // +datum=WGS84). Target: whatever the net uses.
        m_pj = proj_create_crs_to_crs(
            m_ctx,
            "+proj=longlat +datum=WGS84 +no_defs",
            proj4.c_str(),
            nullptr);
        if (!m_pj) return;
        // proj_create_crs_to_crs may need normalizeForVisualization so
        // input order is consistently (lon, lat). Without this, some
        // datasets switch to (lat, lon) and the math silently drifts.
        PJ* nrm = proj_normalize_for_visualization(m_ctx, m_pj);
        if (nrm) {
            proj_destroy(m_pj);
            m_pj = nrm;
        }
    }

    ~GeoInverter() {
        if (m_pj)  proj_destroy(m_pj);
        if (m_ctx) proj_context_destroy(m_ctx);
    }

    GeoInverter(const GeoInverter&)            = delete;
    GeoInverter& operator=(const GeoInverter&) = delete;

    [[nodiscard]] bool ok() const noexcept { return m_pj != nullptr; }

    // Batched lon/lat → SUMO XY transform. Operates in-place on a packed
    // [x0,y0,x1,y1,...] buffer holding `nPoints` points. Returns false on
    // any PROJ error; on success, the buffer is fully transformed.
    bool transformInPlace(double* xy, std::size_t nPoints) const {
        if (nPoints == 0) return true;
        // Stride is 2 doubles == sizeof(double)*2 bytes between successive
        // x's (and y's). proj_trans_generic walks both arrays independently
        // so passing the same base + half-element offset is the standard
        // idiom for an interleaved [x,y] buffer.
        const size_t stride = sizeof(double) * 2;
        const size_t done = proj_trans_generic(
            m_pj, PJ_FWD,
            xy,     stride, nPoints,
            xy + 1, stride, nPoints,
            nullptr, 0, 0,
            nullptr, 0, 0);
        if (done != nPoints || proj_context_errno(m_ctx) != 0) {
            proj_errno_reset(m_pj);
            return false;
        }
        // Apply the constant netOffset shift after the projection. Doing
        // it in a tight loop here is negligible compared to the PROJ math.
        for (std::size_t i = 0; i < nPoints; ++i) {
            xy[i * 2]     += m_ox;
            xy[i * 2 + 1] += m_oy;
        }
        return true;
    }

private:
    PJ_CONTEXT* m_ctx = nullptr;
    PJ*         m_pj  = nullptr;
    double      m_ox  = 0.0;
    double      m_oy  = 0.0;
};

// Append `n` points starting at `src` to `dst_points` (as float32 pairs)
// and grow the running bbox. `src` is f64 X,Y,X,Y,…  — already in SUMO XY
// (caller is responsible for batch-transforming via GeoInverter first).
void copyPointsToFloat(const double* src, std::size_t nPoints,
                       std::vector<float>& dst,
                       float& minX, float& minY,
                       float& maxX, float& maxY) {
    // NOTE: callers MUST pre-reserve `dst` to the final total. Calling
    // reserve() per-lane was O(n²) on Berlin (728k lanes → 728k reallocs
    // of the growing buffer, each copying everything previously written).
    for (std::size_t i = 0; i < nPoints; ++i) {
        const float x = static_cast<float>(src[i * 2]);
        const float y = static_cast<float>(src[i * 2 + 1]);
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
    const fs::path dir = p.parent_path() / "__ecaldeck__";
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

    // Geo-referenced caches hold lon/lat (publisher converted XY → lon/lat
    // for the deck.gl frontend). Construct a PROJ inverter so we can map
    // back to SUMO XY — the qt renderer is XY-only, and live vehicle
    // positions arriving later from libsumo are in XY.
    std::unique_ptr<GeoInverter> geoInv;
    if (pb.geo_referenced()) {
        const std::string& proj4 = pb.proj_parameter();
        if (proj4.empty() || proj4 == "!") {
            std::fprintf(stderr,
                "[NetworkCache] %s: geo_referenced=true but proj_parameter is empty/'!' — falling back to libsumo\n",
                cacheFile.c_str());
            return nullptr;
        }
        // net_offset is "x,y" (decimal degrees not involved — these are
        // meters that GeoConvHelper subtracted in cartesian2geo).
        double ox = 0.0, oy = 0.0;
        const std::string& nofs = pb.net_offset();
        if (auto comma = nofs.find(','); comma != std::string::npos) {
            try {
                ox = std::stod(nofs.substr(0, comma));
                oy = std::stod(nofs.substr(comma + 1));
            } catch (const std::exception&) {
                std::fprintf(stderr,
                    "[NetworkCache] %s: malformed net_offset \"%s\" — falling back to libsumo\n",
                    cacheFile.c_str(), nofs.c_str());
                return nullptr;
            }
        }
        std::fprintf(stderr, "[NetworkCache] initializing PROJ (proj=\"%s\", offset=%g,%g)\n",
                     proj4.c_str(), ox, oy);
        geoInv = std::make_unique<GeoInverter>(proj4, ox, oy);
        if (!geoInv->ok()) {
            std::fprintf(stderr,
                "[NetworkCache] %s: failed to init PROJ from \"%s\" — falling back to libsumo\n",
                cacheFile.c_str(), proj4.c_str());
            return nullptr;
        }
        std::fprintf(stderr, "[NetworkCache] PROJ ready\n");
    }
    const GeoInverter* inv = geoInv.get();

    auto ng = std::make_shared<NetworkGeometry>();

    float minX = std::numeric_limits<float>::infinity();
    float minY = std::numeric_limits<float>::infinity();
    float maxX = -std::numeric_limits<float>::infinity();
    float maxY = -std::numeric_limits<float>::infinity();

    // ---- Lanes ----
    auto              lanePos    = unpack<double>(pb.lane_positions());
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

    std::fprintf(stderr, "[NetworkCache] lanes: batch PROJ transform on %zu points\n", lanePos.size() / 2);
    // Batched lon/lat → SUMO XY for the whole lane buffer in one PROJ call.
    if (inv && !inv->transformInPlace(lanePos.data(), lanePos.size() / 2)) {
        std::fprintf(stderr,
            "[NetworkCache] %s: PROJ batch transform failed on lane buffer — "
            "falling back to libsumo\n", cacheFile.c_str());
        return nullptr;
    }
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
    auto              juncPos    = unpack<double>(pb.junction_positions());
    const auto        juncStarts = unpack<std::uint32_t>(pb.junction_starts());
    const std::size_t nJunc      = static_cast<std::size_t>(pb.junction_ids_size());
    if (juncStarts.size() != nJunc + 1) return nullptr;
    if (inv && !inv->transformInPlace(juncPos.data(), juncPos.size() / 2)) {
        std::fprintf(stderr,
            "[NetworkCache] %s: PROJ batch transform failed on junction buffer — "
            "falling back to libsumo\n", cacheFile.c_str());
        return nullptr;
    }

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
    // tls_positions: 4 doubles per bar (x1,y1,x2,y2).  tls_entries:
    // one entry per bar, parallel.  Convert (x1,y1)->(x2,y2) into the
    // qt format (center + lane forward tangent + width).
    auto              tlsPos = unpack<double>(pb.tls_positions());
    const std::size_t nTls   = static_cast<std::size_t>(pb.tls_entries_size());
    if (tlsPos.size() == nTls * 4) {
        // 2 endpoints per bar → nTls*2 points; one batched call.
        if (inv && !inv->transformInPlace(tlsPos.data(), nTls * 2)) {
            std::fprintf(stderr,
                "[NetworkCache] %s: PROJ batch transform failed on TLS buffer — "
                "falling back to libsumo\n", cacheFile.c_str());
            return nullptr;
        }
        ng->tls_ids.reserve(nTls);
        ng->tls_state_index.reserve(nTls);
        ng->tls_x.reserve(nTls);
        ng->tls_y.reserve(nTls);
        ng->tls_dx.reserve(nTls);
        ng->tls_dy.reserve(nTls);
        ng->tls_w.reserve(nTls);
        for (std::size_t i = 0; i < nTls; ++i) {
            const double x1 = tlsPos[i * 4 + 0];
            const double y1 = tlsPos[i * 4 + 1];
            const double x2 = tlsPos[i * 4 + 2];
            const double y2 = tlsPos[i * 4 + 3];
            const double bx = x2 - x1, by = y2 - y1;
            const double w  = std::sqrt(bx * bx + by * by);
            const float  cx = static_cast<float>(0.5 * (x1 + x2));
            const float  cy = static_cast<float>(0.5 * (y1 + y2));
            // Lane forward tangent = perpendicular to bar, unit length.
            // The sign is irrelevant — bar is drawn symmetrically.
            float dx = 1.0f, dy = 0.0f;
            if (w > 1e-9) {
                dx = static_cast<float>(-by / w);
                dy = static_cast<float>( bx / w);
            }
            ng->tls_x.push_back(cx);
            ng->tls_y.push_back(cy);
            ng->tls_dx.push_back(dx);
            ng->tls_dy.push_back(dy);
            ng->tls_w.push_back(static_cast<float>(w));

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
        "bbox=(%.1f,%.1f)-(%.1f,%.1f)%s\n",
        cacheFile.c_str(), ng->lane_count(), ng->junction_count(),
        ng->tls_marker_count(), ng->min_x, ng->min_y, ng->max_x, ng->max_y,
        inv ? " (geo→XY via PROJ)" : "");
    return ng;
}

}  // namespace network_cache
