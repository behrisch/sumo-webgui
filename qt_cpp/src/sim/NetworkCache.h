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
// `ecal_deck/sumo_ecal_publisher.py` (`__ecaldeck__/<base>.net.v<N>.bin`).
// Returns empty if `netFile` is empty.
std::string cachePathFor(const std::string& netFile);

// Try to load a cached `NetworkGeometry` for `sumocfgPath`. Returns
// nullptr on any of:
//   - sumocfg parse error / net file missing
//   - cache file missing or unreadable
//   - cache older than the net file (stale)
//   - proto version mismatch
//   - protobuf parse failure
// On hit, the returned struct owns its arrays (copied out of the parsed
// proto). A later refactor may switch to zero-copy views into a
// long-lived parsed-message holder.
//
// THIS IS A STUB. The implementation is intentionally a no-op until the
// caller side (SimWorker phase-split) is in place; see "Shared `.pb`
// network cache + early-render" in PLAN.md.
std::shared_ptr<NetworkGeometry> tryLoadCache(const std::string& sumocfgPath);

}  // namespace network_cache
