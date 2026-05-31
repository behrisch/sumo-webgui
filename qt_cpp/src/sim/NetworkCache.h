#pragma once

#include <memory>
#include <string>

struct NetworkGeometry;

namespace network_cache {

// Resolve the .net.xml referenced by `sumocfgPath`. Returns empty on
// parse failure. Mirrors the lookup Python's `_cache_path` does, so both
// halves of the project agree on which .pb file backs which sumocfg.
std::string resolveNetFile(const std::string& sumocfgPath);

// Cache path for `netFile`, matching `_cache_path(net_file, 'net')` in
// `ecal_deck/sumo_ecal_publisher.py` (`__sumocache__/<base>.net.v<N>.bin`).
// Returns empty if `netFile` is empty.
std::string cachePathFor(const std::string& netFile);

// Try to load a cached `NetworkGeometry` for `sumocfgPath`. Returns
// nullptr on any of:
//   - sumocfg parse error / net file missing
//   - cache file missing or unreadable
//   - cache older than the net file (stale)
//   - proto version mismatch
//   - protobuf parse failure
//   - geo-referenced network with missing/invalid PROJ string or net offset
//
// Geo-referenced caches are supported: cached lon/lat is inverted to
// SUMO XY via PROJ + the cached proj_parameter and net_offset, so the
// qt renderer (XY-only) sees the same coordinate frame whether the data
// came from the cache or from a live libsumo extraction.
//
// On hit, the returned struct owns its arrays (copied out of the parsed
// proto) and contains lanes + junctions + TLS bars + bbox + lane_kind.
// Polygons / POIs / stopping places / detectors are NOT populated — those
// live in separate cache families (`*.poly.vN.bin`, `*.stops.vN.bin`,
// `*.det.vN.bin`) and still come from the libsumo extraction in
// buildNetworkGeometry(). A future iteration can plug them in.
std::shared_ptr<NetworkGeometry> tryLoadCache(const std::string& sumocfgPath);

}  // namespace network_cache
