#include "SimWorker.h"

#include <QTimer>
#include <cmath>
#include <exception>
#include <string>
#include <vector>

// Qt defines `signals` as a macro; libsumo has a parameter named `signals`.
#pragma push_macro("signals")
#undef signals
#include <libsumo/Lane.h>
#include <libsumo/Person.h>
#include <libsumo/Simulation.h>
#include <libsumo/TrafficLight.h>
#include <libsumo/Vehicle.h>
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
        const auto ids = libsumo::Vehicle::getIDList();
        const std::size_t n = ids.size();
        snap->ids.reserve(n);
        snap->x.reserve(n);
        snap->y.reserve(n);
        snap->cos_a.reserve(n);
        snap->sin_a.reserve(n);
        snap->rgba.reserve(n * 4);

        for (const auto& id : ids) {
            libsumo::TraCIPosition p;
            try { p = libsumo::Vehicle::getPosition(id); }
            catch (...) { continue; }
            double ang = 90.0;
            try { ang = libsumo::Vehicle::getAngle(id); } catch (...) {}

            // SUMO angle: degrees clockwise from north. Convert to a math
            // angle in the XY plane (counter-clockwise from +x).
            const double mathAngRad = (90.0 - ang) * M_PI / 180.0;

            libsumo::TraCIColor c(255, 255, 0, 255);
            try {
                const std::string t = libsumo::Vehicle::getTypeID(id);
                c = libsumo::VehicleType::getColor(t);
            } catch (...) {}

            snap->ids.push_back(id);
            snap->x.push_back(static_cast<float>(p.x));
            snap->y.push_back(static_cast<float>(p.y));
            snap->cos_a.push_back(static_cast<float>(std::cos(mathAngRad)));
            snap->sin_a.push_back(static_cast<float>(std::sin(mathAngRad)));
            snap->rgba.push_back(static_cast<std::uint8_t>(c.r));
            snap->rgba.push_back(static_cast<std::uint8_t>(c.g));
            snap->rgba.push_back(static_cast<std::uint8_t>(c.b));
            snap->rgba.push_back(static_cast<std::uint8_t>(c.a));
        }

        // Persons.
        const auto pids = libsumo::Person::getIDList();
        snap->person_ids.reserve(pids.size());
        snap->person_x.reserve(pids.size());
        snap->person_y.reserve(pids.size());
        snap->person_rgba.reserve(pids.size() * 4);
        for (const auto& id : pids) {
            libsumo::TraCIPosition p;
            try { p = libsumo::Person::getPosition(id); }
            catch (...) { continue; }
            libsumo::TraCIColor c(255, 200, 0, 255);
            try {
                const std::string t = libsumo::Person::getTypeID(id);
                c = libsumo::VehicleType::getColor(t);
            } catch (...) {}
            snap->person_ids.push_back(id);
            snap->person_x.push_back(static_cast<float>(p.x));
            snap->person_y.push_back(static_cast<float>(p.y));
            snap->person_rgba.push_back(static_cast<std::uint8_t>(c.r));
            snap->person_rgba.push_back(static_cast<std::uint8_t>(c.g));
            snap->person_rgba.push_back(static_cast<std::uint8_t>(c.b));
            snap->person_rgba.push_back(static_cast<std::uint8_t>(c.a));
        }

        // Traffic-light states.
        const auto tlsIds = libsumo::TrafficLight::getIDList();
        for (const auto& tid : tlsIds) {
            try {
                snap->tls_states.emplace(
                    tid, libsumo::TrafficLight::getRedYellowGreenState(tid));
            } catch (...) {}
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
