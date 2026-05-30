#include "SimWorker.h"

#include <QTimer>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

// Qt defines `signals` as a macro; libsumo has a parameter named `signals`.
#pragma push_macro("signals")
#undef signals
#include <libsumo/Batch.h>
#include <libsumo/Lane.h>
#include <libsumo/Simulation.h>
#include <libsumo/VehicleType.h>
#pragma pop_macro("signals")

#include "NetworkGeometry.h"

SimWorker::SimWorker(QObject* parent) : QObject(parent) {
    qRegisterMetaType<std::shared_ptr<NetworkGeometry>>("std::shared_ptr<NetworkGeometry>");
    qRegisterMetaType<SimSnapshotPtr>("SimSnapshotPtr");

    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &SimWorker::onTimerTick);
}

SimWorker::~SimWorker() {
    closeIfOpen();
}

void SimWorker::closeIfOpen() noexcept {
    if (m_timer) m_timer->stop();
    m_playing = false;
    if (!m_open) return;
    try {
        libsumo::Simulation::close();
    } catch (...) {
        // ignore on destructor path
    }
    m_open = false;
    m_stepCount = 0;
}

void SimWorker::loadScenario(const QString& sumocfgPath) {
    closeIfOpen();
    try {
        const std::vector<std::string> cmd = {
            "sumo",
            "-c", sumocfgPath.toStdString(),
            "--no-step-log", "true",
            "--no-warnings", "true",
        };
        libsumo::Simulation::start(cmd);
        m_open = true;
        m_stepCount = 0;
        m_skippedSnapshots.store(0, std::memory_order_release);
        m_renderPending->store(0, std::memory_order_release);
        m_bmWindowStartNs = m_bmWindowSteps = m_bmWindowSnapshots = 0;
        m_bmWindowSkipped = m_bmWindowStepNs = m_bmWindowBuildNs = 0;
        m_typeColors.clear();  // fresh per-scenario type registry
        emit scenarioLoaded(sumocfgPath);
        m_ng = buildNetworkGeometry();
        emit networkReady(m_ng);
        // First step so the user has vehicles to look at.
        stepOnce();
    } catch (const std::exception& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
    } catch (...) {
        emit errorOccurred(QStringLiteral("Unknown error starting libsumo"));
    }
}

SimSnapshotPtr SimWorker::buildSnapshot() {
    auto snap = std::make_shared<SimSnapshot>();
    snap->stepIdx = m_stepCount;
    snap->simTime = libsumo::Simulation::getTime();

    try {
        // ---- in-engine batched extraction (single C++ loop per section) ----
        // For vehicle attribute coloring (waiting time / CO2 / fuel) we ask
        // Batch to fill the corresponding column; speed is always populated.
        std::vector<std::string> vehAttrs;
        switch (m_vehicleColorMode) {
            case 2: vehAttrs = {"waiting_time"};     break;
            case 3: vehAttrs = {"co2_emission"};     break;
            case 4: vehAttrs = {"fuel_consumption"}; break;
            default: break;
        }
        libsumo::Batch::beginStep();
        if (m_vehiclesVisible) {
            libsumo::Batch::fillVehicles(vehAttrs, /*geoReferenced=*/false);
        }
        if (m_agentsVisible) {
            libsumo::Batch::fillAgents(/*geoReferenced=*/false);
        }
        if (m_tlsVisible) {
            libsumo::Batch::fillTLS();
        }
        const libsumo::BatchBuffers& buf = libsumo::Batch::buffers();

        // Grow color cache to cover any newly registered types.  Lazy fill on
        // first sighting of an index avoids paying VehicleType::getColor for
        // types that aren't currently visible.
        const std::uint32_t nTypes = libsumo::Batch::typeCount();
        if (m_typeColors.size() < nTypes) {
            m_typeColors.resize(nTypes, TypeColor{255, 255, 0, 255, false, 5.0f});
        }
        auto colorForType = [&](std::uint32_t idx) -> TypeColor& {
            TypeColor& tc = m_typeColors[idx];
            if (!tc.set) {
                try {
                    const std::string tid = libsumo::Batch::typeId(idx);
                    const libsumo::TraCIColor c = libsumo::VehicleType::getColor(tid);
                    const double len = libsumo::VehicleType::getLength(tid);
                    tc = TypeColor{static_cast<std::uint8_t>(c.r),
                                   static_cast<std::uint8_t>(c.g),
                                   static_cast<std::uint8_t>(c.b),
                                   static_cast<std::uint8_t>(c.a), true,
                                   static_cast<float>(len)};
                } catch (...) {
                    tc.set = true;  // give up — keep the default yellow / 5 m
                }
            }
            return tc;
        };

        // Helper: split a packed null-terminated id blob into a target vector.
        auto splitIds = [](const std::string& blob, std::size_t count,
                           std::vector<std::string>& out) {
            out.clear();
            out.reserve(count);
            const char* p   = blob.data();
            const char* end = p + blob.size();
            for (std::size_t i = 0; i < count && p < end; ++i) {
                const std::size_t len = std::strlen(p);
                out.emplace_back(p, len);
                p += len + 1;
            }
        };

        // ---- vehicles -----------------------------------------------------
        // Hand the f64 positions and f32 angles to the snapshot as raw bytes;
        // VehicleLayer uploads them straight to a VBO (GL_DOUBLE stride 24
        // for vec2 position; GL_FLOAT for angle, cos/sin computed in shader).
        const std::uint32_t N = buf.veh_count;
        const auto* tidxBuf = reinterpret_cast<const std::uint32_t*>(buf.veh_type_indices.data());
        const auto* speedBuf = reinterpret_cast<const float*>(buf.veh_speeds.data());
        const auto* attrBuf = buf.veh_attr_count > 0
            ? reinterpret_cast<const float*>(buf.veh_attr_vals.data()) : nullptr;
        snap->veh_positions = buf.veh_positions;
        snap->veh_angles    = buf.veh_angles;
        snap->veh_speeds    = buf.veh_speeds;
        snap->rgba.resize(N * 4);
        snap->veh_lengths.resize(N);
        snap->veh_type_indices.assign(tidxBuf, tidxBuf + N);

        // Pick the dynamic-coloring source: speed (m/s), waiting_time (s),
        // co2 (mg/s), or fuel (ml/s).  For "Type" mode we keep the static
        // per-type color.  Values are normalized to [0,1] against a sensible
        // upper bound and mapped through a viridis-ish ramp.
        auto viridis = [](float t, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
            if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
            // 4-stop ramp: dark blue -> teal -> green -> yellow.
            static const float stops[5][3] = {
                {68/255.f, 1/255.f, 84/255.f},
                {59/255.f, 82/255.f, 139/255.f},
                {33/255.f, 145/255.f, 140/255.f},
                {94/255.f, 201/255.f, 98/255.f},
                {253/255.f, 231/255.f, 37/255.f},
            };
            const float s = t * 4.0f;
            const int i = std::min(3, static_cast<int>(s));
            const float u = s - i;
            const float R = stops[i][0] * (1 - u) + stops[i + 1][0] * u;
            const float G = stops[i][1] * (1 - u) + stops[i + 1][1] * u;
            const float B = stops[i][2] * (1 - u) + stops[i + 1][2] * u;
            r = static_cast<std::uint8_t>(R * 255);
            g = static_cast<std::uint8_t>(G * 255);
            b = static_cast<std::uint8_t>(B * 255);
        };
        // Per-mode normalization caps. Speed: 50 m/s (~180 km/h). Waiting:
        // 60 s. CO2: 5000 mg/s. Fuel: 5 ml/s.
        float cap = 1.0f;
        switch (m_vehicleColorMode) {
            case 1: cap = 50.0f;   break;
            case 2: cap = 60.0f;   break;
            case 3: cap = 5000.0f; break;
            case 4: cap = 5.0f;    break;
        }
        for (std::uint32_t i = 0; i < N; ++i) {
            const TypeColor& tc = colorForType(tidxBuf[i]);
            snap->veh_lengths[i] = tc.length;
            std::uint8_t r = tc.r, g = tc.g, b = tc.b;
            if (m_vehicleColorMode != 0) {
                float v = 0.0f;
                if (m_vehicleColorMode == 1 && speedBuf) v = speedBuf[i];
                else if (attrBuf) v = attrBuf[i];
                viridis(v / cap, r, g, b);
            }
            snap->rgba[i * 4 + 0] = r;
            snap->rgba[i * 4 + 1] = g;
            snap->rgba[i * 4 + 2] = b;
            snap->rgba[i * 4 + 3] = tc.a;
        }
        splitIds(buf.veh_ids, N, snap->ids);

        // Type-id table for picking / display (one entry per registered type).
        const std::uint32_t nTypesNow = libsumo::Batch::typeCount();
        snap->type_ids.resize(nTypesNow);
        for (std::uint32_t i = 0; i < nTypesNow; ++i) {
            try { snap->type_ids[i] = libsumo::Batch::typeId(i); }
            catch (...) { snap->type_ids[i].clear(); }
        }

        // ---- agents (persons + containers; rendered the same way) ---------
        const std::uint32_t M = buf.agent_count;
        const auto* aTidxBuf = reinterpret_cast<const std::uint32_t*>(buf.agent_type_indices.data());
        snap->agent_positions = buf.agent_positions;
        snap->person_rgba.resize(M * 4);
        for (std::uint32_t i = 0; i < M; ++i) {
            const TypeColor& tc = colorForType(aTidxBuf[i]);
            snap->person_rgba[i * 4 + 0] = tc.r;
            snap->person_rgba[i * 4 + 1] = tc.g;
            snap->person_rgba[i * 4 + 2] = tc.b;
            snap->person_rgba[i * 4 + 3] = tc.a;
        }
        splitIds(buf.agent_ids, M, snap->person_ids);

        // ---- TLS states ---------------------------------------------------
        {
            const char* idP   = buf.tls_ids.data();
            const char* idEnd = idP + buf.tls_ids.size();
            const char* stP   = buf.tls_states.data();
            const char* stEnd = stP + buf.tls_states.size();
            const std::uint32_t T = buf.tls_count;
            for (std::uint32_t i = 0; i < T && idP < idEnd && stP < stEnd; ++i) {
                const std::size_t lid = std::strlen(idP);
                const std::size_t lst = std::strlen(stP);
                snap->tls_states.emplace(std::string(idP, lid), std::string(stP, lst));
                idP += lid + 1;
                stP += lst + 1;
            }
        }

        // Edge attribute coloring (per-lane). Skipped when mode==None to
        // keep step cost low on big networks.
        if (m_colorMode != 0 && m_edgeDataVisible && m_ng) {
            const std::size_t nLanes = m_ng->lane_count();
            snap->lane_attr_rgba.assign(nLanes * 4, 0);
            auto setCol = [&](std::size_t i, float r, float g, float b) {
                snap->lane_attr_rgba[i * 4]     = static_cast<std::uint8_t>(r * 255);
                snap->lane_attr_rgba[i * 4 + 1] = static_cast<std::uint8_t>(g * 255);
                snap->lane_attr_rgba[i * 4 + 2] = static_cast<std::uint8_t>(b * 255);
                snap->lane_attr_rgba[i * 4 + 3] = 220;
            };
            auto ramp = [](float t, float& r, float& g, float& b) {
                // 0=red, 0.5=yellow, 1=green. Clamp & smooth.
                if (t < 0) t = 0; if (t > 1) t = 1;
                if (t < 0.5f) {
                    const float u = t * 2.0f;
                    r = 1.0f; g = u; b = 0.0f;
                } else {
                    const float u = (t - 0.5f) * 2.0f;
                    r = 1.0f - u; g = 1.0f; b = 0.0f;
                }
            };
            for (std::size_t i = 0; i < nLanes; ++i) {
                const auto& id = m_ng->lane_ids[i];
                float t = 0.0f;
                try {
                    switch (m_colorMode) {
                        case 1: {  // mean speed normalized by allowed max.
                            const double v = libsumo::Lane::getLastStepMeanSpeed(id);
                            const double vmax = libsumo::Lane::getMaxSpeed(id);
                            t = vmax > 0.1 ? static_cast<float>(v / vmax) : 0.0f;
                            break;
                        }
                        case 2: {  // occupancy 0..1 (already a fraction)
                            t = 1.0f - static_cast<float>(
                                libsumo::Lane::getLastStepOccupancy(id));
                            break;
                        }
                        case 3: {  // halting vehicles count, log scale up to 10.
                            const int h = libsumo::Lane::getLastStepHaltingNumber(id);
                            t = 1.0f - std::min(1.0f, std::log10(1.0f + h) / 1.0f);
                            break;
                        }
                    }
                } catch (...) { t = 0.0f; }
                float r, g, b; ramp(t, r, g, b);
                setCol(i, r, g, b);
            }
            switch (m_colorMode) {
                case 1: snap->lane_attr_label = "mean speed (red=slow, green=at limit)"; break;
                case 2: snap->lane_attr_label = "occupancy (red=full, green=empty)";     break;
                case 3: snap->lane_attr_label = "halting count (red=many, green=none)";  break;
            }
        }
    } catch (const std::exception& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
    }
    return snap;
}

void SimWorker::stepOnce() {
    if (!m_open) return;
    try {
        const auto t0 = std::chrono::steady_clock::now();
        libsumo::Simulation::step();
        const auto t1 = std::chrono::steady_clock::now();
        ++m_stepCount;

        // Skip extraction if either (a) the GUI hasn't consumed the previous
        // snapshot yet, or (b) nothing visual is currently active — same idea
        // as sumo-gui only asking libsumo for what it draws.  The simulation
        // step itself still runs so wall-time progress matches the ecal_deck
        // publisher under the same conditions.
        const bool anyLayer = m_vehiclesVisible || m_agentsVisible
                           || m_tlsVisible || m_edgeDataVisible;
        const bool needData = m_windowVisible && anyLayer;
        const bool pending  = m_backpressure
            && m_renderPending->load(std::memory_order_acquire) != 0;
        qint64 buildNs = 0;
        bool   built   = false;
        if (!needData || pending) {
            ++m_skippedSnapshots;
        } else {
            const auto b0 = std::chrono::steady_clock::now();
            auto snap = buildSnapshot();
            const auto b1 = std::chrono::steady_clock::now();
            buildNs = std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count();
            built = true;
            m_renderPending->store(1, std::memory_order_release);
            emit snapshotReady(std::move(snap));
        }
        emit stepReady(m_stepCount, libsumo::Simulation::getTime());

        // ---- rolling benchmark accumulation / report ----
        const qint64 nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (m_bmWindowStartNs == 0) m_bmWindowStartNs = nowNs;
        ++m_bmWindowSteps;
        if (built) ++m_bmWindowSnapshots; else ++m_bmWindowSkipped;
        m_bmWindowStepNs  += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        m_bmWindowBuildNs += buildNs;
        constexpr qint64 kReportNs = 2'000'000'000;  // 2 s
        const qint64 dtNs = nowNs - m_bmWindowStartNs;
        if (dtNs >= kReportNs) {
            const double secs = dtNs / 1e9;
            const double stepsPerSec = m_bmWindowSteps / secs;
            const double snapsPerSec = m_bmWindowSnapshots / secs;
            const double skipRate = m_bmWindowSteps > 0
                ? double(m_bmWindowSkipped) / double(m_bmWindowSteps) : 0.0;
            const double avgStepMs = m_bmWindowSteps > 0
                ? (m_bmWindowStepNs / 1e6) / double(m_bmWindowSteps) : 0.0;
            const double avgBuildMs = m_bmWindowSnapshots > 0
                ? (m_bmWindowBuildNs / 1e6) / double(m_bmWindowSnapshots) : 0.0;
            std::fprintf(stderr,
                "[bench] steps/s=%.1f snapshots/s=%.1f skip=%.1f%% "
                "avg_step=%.2fms avg_build=%.2fms\n",
                stepsPerSec, snapsPerSec, skipRate * 100.0,
                avgStepMs, avgBuildMs);
            std::fflush(stderr);
            emit benchmarkReport(stepsPerSec, snapsPerSec, skipRate,
                                 avgStepMs, avgBuildMs);
            m_bmWindowStartNs   = nowNs;
            m_bmWindowSteps     = 0;
            m_bmWindowSnapshots = 0;
            m_bmWindowSkipped   = 0;
            m_bmWindowStepNs    = 0;
            m_bmWindowBuildNs   = 0;
        }
    } catch (const std::exception& e) {
        emit errorOccurred(QString::fromUtf8(e.what()));
        pause();
    }
}

void SimWorker::play() {
    if (!m_open || m_playing) return;
    m_playing = true;
    m_timer->start(m_delayMs);
}

void SimWorker::pause() {
    m_playing = false;
    if (m_timer) m_timer->stop();
}

void SimWorker::setDelayMs(int ms) {
    m_delayMs = std::max(0, ms);
    if (m_playing && m_timer) m_timer->start(m_delayMs);
}

void SimWorker::onTimerTick() {
    if (m_playing) stepOnce();
}

void SimWorker::setColorMode(int mode) {
    m_colorMode = mode;
}

void SimWorker::setVehicleColorMode(int mode) {
    m_vehicleColorMode = mode;
}

void SimWorker::setBackpressure(bool on) {
    m_backpressure = on;
    if (!on) m_renderPending->store(0, std::memory_order_release);
}

void SimWorker::setWindowVisible(bool on)   { m_windowVisible   = on; }
void SimWorker::setVehiclesVisible(bool on) { m_vehiclesVisible = on; }
void SimWorker::setAgentsVisible(bool on)   { m_agentsVisible   = on; }
void SimWorker::setTLSVisible(bool on)      { m_tlsVisible      = on; }
void SimWorker::setEdgeDataVisible(bool on) { m_edgeDataVisible = on; }

void SimWorker::shutdown() {
    closeIfOpen();
    emit scenarioClosed();
}
