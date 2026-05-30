#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "SimSnapshot.h"

struct NetworkGeometry;
class QTimer;

class SimWorker : public QObject {
    Q_OBJECT

public:
    explicit SimWorker(QObject* parent = nullptr);
    ~SimWorker() override;

    // Shared "render-pending" flag.  The view stores 0 here when it has
    // consumed the latest snapshot; the worker stores 1 each time it emits
    // a fresh snapshot.  When non-zero, stepOnce() advances the simulation
    // but skips the per-step extraction work (Batch::fillVehicles + the
    // viridis loops + memcpy) — that's how we keep the libsumo step rate
    // comparable to the ecal_deck publisher under back-pressure.
    std::shared_ptr<std::atomic<int>> renderPendingFlag() const { return m_renderPending; }
    // Cumulative count of simulation steps where extraction was skipped due
    // to back-pressure since the current scenario was loaded.  Atomic so the
    // GUI thread can poll it without locking.
    qint64 skippedSnapshots() const {
        return m_skippedSnapshots.load(std::memory_order_relaxed);
    }

public slots:
    void loadScenario(const QString& sumocfgPath);
    void stepOnce();
    void play();
    void pause();
    void setDelayMs(int ms);
    void setColorMode(int mode);  // 0=None, 1=Speed, 2=Occupancy, 3=Halting
    void setVehicleColorMode(int mode);  // 0=Type, 1=Speed, 2=Waiting, 3=CO2, 4=Fuel
    void setBackpressure(bool on);       // skip extraction when render hasn't caught up
    // Section-level visibility gates. When a layer is hidden (or the whole
    // window isn't visible) the worker skips the corresponding Batch::fill*
    // call — same idea as sumo-gui only asking libsumo for what it draws.
    void setWindowVisible(bool on);
    void setVehiclesVisible(bool on);
    void setAgentsVisible(bool on);
    void setTLSVisible(bool on);
    void setEdgeDataVisible(bool on);    // per-lane color overlay (the colorMode loop)
    void shutdown();

signals:
    void stepReady(qint64 stepCount, double simTime);
    void errorOccurred(const QString& message);
    void scenarioLoaded(const QString& sumocfgPath);
    void scenarioClosed();
    void networkReady(std::shared_ptr<NetworkGeometry> ng);
    void snapshotReady(SimSnapshotPtr snap);
    // Periodic benchmark report (~ every 2 s wall time). Numbers are over
    // the last reporting window: steps/s, snapshots/s, skip rate, average
    // wall-time per sim step (ms) and per snapshot build (ms).
    void benchmarkReport(double stepsPerSec, double snapshotsPerSec,
                         double skipRate, double avgStepMs,
                         double avgBuildMs);

private slots:
    void onTimerTick();

private:
    void closeIfOpen() noexcept;
    SimSnapshotPtr buildSnapshot();

    // RGBA color cache keyed by libsumo::Batch type-id index.  Filled lazily
    // on first sighting of an index via a single VehicleType::getColor call.
    // Resized to match Batch::typeCount() at the top of each buildSnapshot.
    struct TypeColor { std::uint8_t r, g, b, a; bool set; float length; };
    std::vector<TypeColor> m_typeColors;

    QTimer* m_timer  = nullptr;
    int     m_delayMs = 0;       // 0 = step as fast as possible
    bool    m_open       = false;
    bool    m_playing    = false;
    qint64  m_stepCount  = 0;
    int     m_colorMode  = 0;
    int     m_vehicleColorMode = 0;
    bool    m_backpressure = true;
    bool    m_windowVisible   = true;
    bool    m_vehiclesVisible = true;
    bool    m_agentsVisible   = true;
    bool    m_tlsVisible      = true;
    bool    m_edgeDataVisible = true;
    std::atomic<qint64> m_skippedSnapshots{0};

    // Rolling benchmark window (wall-time bucketed every ~2 s).
    qint64 m_bmWindowStartNs    = 0;
    qint64 m_bmWindowSteps      = 0;
    qint64 m_bmWindowSnapshots  = 0;
    qint64 m_bmWindowSkipped    = 0;
    qint64 m_bmWindowStepNs     = 0;  // sum of Simulation::step() wall-time
    qint64 m_bmWindowBuildNs    = 0;  // sum of buildSnapshot() wall-time

    std::shared_ptr<std::atomic<int>> m_renderPending = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<NetworkGeometry> m_ng;
};
