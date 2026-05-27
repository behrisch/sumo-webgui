#include "MainWindow.h"

#include <QAction>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QThread>
#include <QToolBar>

#include "NetworkView.h"
#include "sim/SimWorker.h"

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    m_view = new NetworkView(this);
    setCentralWidget(m_view);

    buildMenusAndToolbar();
    buildStatusBar();

    m_simThread = new QThread(this);
    m_sim = new SimWorker();
    m_sim->moveToThread(m_simThread);
    connect(m_simThread, &QThread::finished, m_sim, &QObject::deleteLater);
    connect(m_sim, &SimWorker::stepReady, this, &MainWindow::onSimReady);
    connect(m_sim, &SimWorker::errorOccurred, this, &MainWindow::onSimError);
    connect(m_sim, &SimWorker::networkReady, m_view, &NetworkView::setNetwork);
    connect(m_sim, &SimWorker::snapshotReady, m_view, &NetworkView::setSnapshot);
    connect(m_view, &NetworkView::fpsUpdated, this, &MainWindow::onFps);
    m_simThread->start();
}

MainWindow::~MainWindow() {
    if (m_simThread) {
        QMetaObject::invokeMethod(m_sim, "shutdown", Qt::BlockingQueuedConnection);
        m_simThread->quit();
        m_simThread->wait();
    }
}

void MainWindow::buildMenusAndToolbar() {
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    auto* openAct = fileMenu->addAction(tr("&Open .sumocfg…"));
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::onOpenFile);
    fileMenu->addSeparator();
    auto* quitAct = fileMenu->addAction(tr("&Quit"));
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, this, &QWidget::close);

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    auto* resetAct = viewMenu->addAction(tr("&Reset view"));
    resetAct->setShortcut(QKeySequence(tr("Ctrl+0")));
    connect(resetAct, &QAction::triggered, m_view, &NetworkView::resetView);

    auto* toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->addAction(openAct);
    toolbar->addSeparator();

    m_playAct = toolbar->addAction(
        style()->standardIcon(QStyle::SP_MediaPlay), tr("&Play"));
    m_playAct->setShortcut(QKeySequence(tr("Space")));
    connect(m_playAct, &QAction::triggered, this, &MainWindow::onPlay);

    m_pauseAct = toolbar->addAction(
        style()->standardIcon(QStyle::SP_MediaPause), tr("Pa&use"));
    connect(m_pauseAct, &QAction::triggered, this, &MainWindow::onPause);

    m_stepAct = toolbar->addAction(
        style()->standardIcon(QStyle::SP_MediaSkipForward), tr("&Step"));
    m_stepAct->setShortcut(QKeySequence(tr("S")));
    connect(m_stepAct, &QAction::triggered, this, &MainWindow::onStep);

    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(tr("  Delay (ms): "), this));
    m_delay = new QSlider(Qt::Horizontal, this);
    m_delay->setRange(0, 1000);
    m_delay->setValue(0);
    m_delay->setFixedWidth(160);
    toolbar->addWidget(m_delay);
    connect(m_delay, &QSlider::valueChanged, this, &MainWindow::onDelayChanged);

    toolbar->addSeparator();
    toolbar->addAction(resetAct);
}

void MainWindow::buildStatusBar() {
    m_status = new QLabel(tr("Ready."), this);
    statusBar()->addWidget(m_status, 1);
    m_fpsLbl = new QLabel(tr("— fps"), this);
    statusBar()->addPermanentWidget(m_fpsLbl);
}

void MainWindow::onOpenFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open SUMO configuration"),
        QString(), tr("SUMO configurations (*.sumocfg);;All files (*)"));
    if (!path.isEmpty()) {
        loadSumocfg(path);
    }
}

void MainWindow::loadSumocfg(const QString& path) {
    m_status->setText(tr("Loading %1…").arg(path));
    QMetaObject::invokeMethod(
        m_sim, "loadScenario", Qt::QueuedConnection, Q_ARG(QString, path));
}

void MainWindow::onPlay() {
    QMetaObject::invokeMethod(m_sim, "play", Qt::QueuedConnection);
}

void MainWindow::onPause() {
    QMetaObject::invokeMethod(m_sim, "pause", Qt::QueuedConnection);
}

void MainWindow::onStep() {
    QMetaObject::invokeMethod(m_sim, "stepOnce", Qt::QueuedConnection);
}

void MainWindow::onDelayChanged(int ms) {
    QMetaObject::invokeMethod(
        m_sim, "setDelayMs", Qt::QueuedConnection, Q_ARG(int, ms));
}

void MainWindow::onSimReady(qint64 stepCount, double simTime) {
    m_status->setText(tr("step=%1   t=%2 s").arg(stepCount).arg(simTime, 0, 'f', 2));
}

void MainWindow::onSimError(const QString& message) {
    m_status->setText(tr("Error: %1").arg(message));
}

void MainWindow::onFps(double fps) {
    m_fpsLbl->setText(tr("%1 fps").arg(fps, 0, 'f', 1));
}
