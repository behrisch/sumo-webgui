#include "SimWorker.h"

#include <QTimer>
#include <cmath>
#include <exception>
#include <string>
#include <vector>

// Qt defines `signals` as a macro; libsumo has a parameter named `signals`.
#pragma push_macro("signals")
#undef signals
#include <libsumo/Simulation.h>
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
        emit networkReady(buildNetworkGeometry());
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

void SimWorker::shutdown() {
    closeIfOpen();
    emit scenarioClosed();
}
