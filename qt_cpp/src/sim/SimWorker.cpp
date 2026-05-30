#include "SimWorker.h"

#include <QTimer>
#include <cmath>
#include <cstdint>
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
        // Vehicle/agent attribute lists are empty: the Qt GUI doesn't paint
        // by per-vehicle attributes, only by per-lane edge data (handled
        // separately below via Lane::getLastStepMeanSpeed etc).  Passing
        // empty vectors here skips the attribute columns entirely.
        libsumo::Batch::beginStep();
        libsumo::Batch::fillVehicles({}, /*geoReferenced=*/false);
        libsumo::Batch::fillAgents(/*geoReferenced=*/false);
        libsumo::Batch::fillTLS();
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
        snap->veh_positions = buf.veh_positions;
        snap->veh_angles    = buf.veh_angles;
        snap->rgba.resize(N * 4);
        snap->veh_lengths.resize(N);
        for (std::uint32_t i = 0; i < N; ++i) {
            const TypeColor& tc = colorForType(tidxBuf[i]);
            snap->rgba[i * 4 + 0] = tc.r;
            snap->rgba[i * 4 + 1] = tc.g;
            snap->rgba[i * 4 + 2] = tc.b;
            snap->rgba[i * 4 + 3] = tc.a;
            snap->veh_lengths[i]  = tc.length;
        }
        splitIds(buf.veh_ids, N, snap->ids);

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
        if (m_colorMode != 0 && m_ng) {
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
        libsumo::Simulation::step();
        ++m_stepCount;
        emit snapshotReady(buildSnapshot());
        emit stepReady(m_stepCount, libsumo::Simulation::getTime());
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

void SimWorker::shutdown() {
    closeIfOpen();
    emit scenarioClosed();
}
