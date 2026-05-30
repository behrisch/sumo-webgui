#pragma once

#include <QMainWindow>
#include <QString>

class NetworkView;
class SimWorker;
class QThread;
class QLabel;
class QAction;
class QSlider;
class QComboBox;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    void loadSumocfg(const QString& path);

private slots:
    void onOpenFile();
    void onSimReady(qint64 stepCount, double simTime);
    void onSimError(const QString& message);
    void onFps(double fps);
    void onPlay();
    void onPause();
    void onStep();
    void onDelayChanged(int ms);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    void buildMenusAndToolbar();
    void buildStatusBar();

    NetworkView* m_view = nullptr;

    QThread*   m_simThread = nullptr;
    SimWorker* m_sim       = nullptr;

    QAction* m_playAct  = nullptr;
    QAction* m_pauseAct = nullptr;
    QAction* m_stepAct  = nullptr;
    QSlider* m_delay    = nullptr;
    QComboBox* m_colorMode = nullptr;

    QLabel* m_status  = nullptr;
    QLabel* m_fpsLbl  = nullptr;
};
