#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>
#include <memory>

#include "SimSnapshot.h"

struct NetworkGeometry;
class QTimer;

class SimWorker : public QObject {
    Q_OBJECT

public:
    explicit SimWorker(QObject* parent = nullptr);
    ~SimWorker() override;

public slots:
    void loadScenario(const QString& sumocfgPath);
    void stepOnce();
    void play();
    void pause();
    void setDelayMs(int ms);
    void shutdown();

signals:
    void stepReady(qint64 stepCount, double simTime);
    void errorOccurred(const QString& message);
    void scenarioLoaded(const QString& sumocfgPath);
    void scenarioClosed();
    void networkReady(std::shared_ptr<NetworkGeometry> ng);
    void snapshotReady(SimSnapshotPtr snap);

private slots:
    void onTimerTick();

private:
    void closeIfOpen() noexcept;
    SimSnapshotPtr buildSnapshot();

    QTimer* m_timer  = nullptr;
    int     m_delayMs = 0;       // 0 = step as fast as possible
    bool    m_open       = false;
    bool    m_playing    = false;
    qint64  m_stepCount  = 0;
};
