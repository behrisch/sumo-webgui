// NetworkCache: read the shared `.pb` network geometry the ecal_deck
// Python publisher writes. STUB — only the path-resolution helpers are
// implemented; tryLoadCache always returns nullptr so the existing
// libsumo-based extraction path remains the only working code until the
// SimWorker phase-split lands. See PLAN.md "Shared `.pb` network cache +
// early-render" for the full design.

#include "NetworkCache.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
// XML parsing for the sumocfg <net-file value="..."/> lookup will be
// added with the real implementation; QtXml/QDomDocument is the obvious
// choice and would need Qt6::Xml added in CMakeLists.txt.

#include "sumo_ecal.pb.h"  // exposed by libsumocpp via FindLibsumo.cmake

#include "NetworkGeometry.h"

namespace fs = std::filesystem;

namespace {

// Cache layout version. MUST match _CACHE_VERSION in
// ecal_deck/sumo_ecal_publisher.py.  Bump in lockstep with any
// incompatible NetworkGeometry change on either side.
constexpr int kCacheVersion = 1;

}  // namespace

namespace network_cache {

std::string resolveNetFile(const std::string& sumocfgPath) {
    // STUB: a full implementation parses sumocfg XML and resolves the
    // <net-file value="..."/> attribute (relative to the sumocfg dir).
    // For now we only handle the easy case where the sumocfg itself is
    // co-located with a single .net.xml.  Until the SimWorker side wires
    // this up, callers always hit the libsumo path anyway.
    (void)sumocfgPath;
    return {};
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
    // STUB: see header.  Once SimWorker's phase-split is in place this
    // will:
    //   1. resolveNetFile(sumocfgPath)
    //   2. cachePathFor(netFile)
    //   3. mtime check (cache newer than netFile)
    //   4. read file -> sumo::NetworkGeometry pb message
    //   5. translate typed-array `bytes` payloads into the existing
    //      NetworkGeometry struct (copy or hold-the-message-as-storage).
    (void)sumocfgPath;
    return nullptr;
}

}  // namespace network_cache
