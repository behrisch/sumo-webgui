// Standalone smoke test for NetworkGeometry. Loads a sumocfg via libsumo,
// builds the geometry struct, and prints counts.
#include <cstdio>
#include <string>
#include <vector>

#pragma push_macro("signals")
#undef signals
#include <libsumo/Simulation.h>
#include <libsumo/TraCIDefs.h>
#pragma pop_macro("signals")

#include "src/sim/NetworkGeometry.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <sumocfg>\n", argv[0]);
        return 2;
    }
    std::setlocale(LC_NUMERIC, "C");
    std::vector<std::string> opts = {"-c", argv[1], "--no-step-log", "true"};
    try { libsumo::Simulation::load(opts); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "load failed: %s\n", e.what()); return 1;
    }
    auto ng = buildNetworkGeometry();
    std::size_t road = 0, rail = 0, side = 0, walk = 0, internal_ = 0;
    for (auto k : ng->lane_kind) {
        switch (k) { case 0: ++road; break; case 1: ++rail; break;
                     case 2: ++side; break; case 3: ++walk; break;
                     case 4: ++internal_; break; }
    }
    std::printf("lanes=%zu (road=%zu rail=%zu sidewalk=%zu walk=%zu internal=%zu)\n",
                ng->lane_count(), road, rail, side, walk, internal_);
    std::printf("junc=%zu poly=%zu poi=%zu tls=%zu stoplines=%zu stops=%zu dets=%zu\n",
                ng->junction_count(), ng->polygon_count(), ng->poi_count(),
                ng->tls_marker_count(), ng->stopline_count(),
                ng->stop_count(), ng->det_count());
    std::size_t mee_in = 0, mee_out = 0;
    for (auto k : ng->det_kind) {
        if (k == 2) ++mee_in; else if (k == 3) ++mee_out;
    }
    std::printf("multientryexit: entries=%zu exits=%zu\n", mee_in, mee_out);
    libsumo::Simulation::close();
    return 0;
}
