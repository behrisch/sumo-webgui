#include "MainWindow.h"

#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QEvent>
#include <QFileDialog>
#include <QHideEvent>
#include <QLabel>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QShowEvent>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QIcon>
#include <QThread>
#include <QToolBar>

#include "NetworkView.h"
#include "sim/LogCapture.h"
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
    connect(m_view, &NetworkView::cursorWorldPos, this, &MainWindow::onCursor);
    connect(m_sim, &SimWorker::benchmarkReport, this, &MainWindow::onBenchmark);
    connect(m_sim->logRouter(), &LogRouter::logged, this, &MainWindow::onLog);
    m_view->setRenderPendingFlag(m_sim->renderPendingFlag());
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

    // Dockable log panel for libsumo MsgHandler output (info/warn/error).
    // Built here so the toggle action can live in the View menu next to
    // Reset View.
    m_logDock = new QDockWidget(tr("Log"), this);
    m_logDock->setObjectName(QStringLiteral("logDock"));
    m_logDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    m_logPanel = new QPlainTextEdit(m_logDock);
    m_logPanel->setReadOnly(true);
    m_logPanel->setMaximumBlockCount(2000);  // cap memory; oldest lines drop
    {
        QFont f = m_logPanel->font();
        f.setFamily(QStringLiteral("monospace"));
        f.setStyleHint(QFont::Monospace);
        m_logPanel->setFont(f);
    }
    m_logDock->setWidget(m_logPanel);
    addDockWidget(Qt::BottomDockWidgetArea, m_logDock);
    viewMenu->addAction(m_logDock->toggleViewAction());

    auto* toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->addAction(openAct);
    toolbar->addSeparator();

    // Hand-draw bright media-control icons so they look identical regardless
    // of Qt version, style or whether an icon theme is installed. QStyle's
    // SP_Media* on Qt 6.4/Fusion renders flat grey glyphs that disappear into
    // the toolbar background; QIcon::fromTheme returns null in environments
    // without an XDG icon theme. A 32x32 explicit pixmap dodges both issues.
    auto makeMediaIcon = [](const QString& kind) {
        QPixmap pm(32, 32);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        if (kind == "play") {
            p.setBrush(QColor(80, 200, 100));
            QPolygonF tri({QPointF(8, 6), QPointF(8, 26), QPointF(26, 16)});
            p.drawPolygon(tri);
        } else if (kind == "pause") {
            p.setBrush(QColor(230, 180, 60));
            p.drawRoundedRect(QRectF( 8, 6,  6, 20), 1.5, 1.5);
            p.drawRoundedRect(QRectF(18, 6,  6, 20), 1.5, 1.5);
        } else /* step */ {
            p.setBrush(QColor(80, 160, 220));
            QPolygonF tri({QPointF(6, 6), QPointF(6, 26), QPointF(20, 16)});
            p.drawPolygon(tri);
            p.drawRoundedRect(QRectF(22, 6, 4, 20), 1.0, 1.0);
        }
        return QIcon(pm);
    };

    m_playAct = toolbar->addAction(makeMediaIcon("play"), tr("&Play"));
    m_playAct->setShortcut(QKeySequence(tr("Space")));
    connect(m_playAct, &QAction::triggered, this, &MainWindow::onPlay);

    m_pauseAct = toolbar->addAction(makeMediaIcon("pause"), tr("Pa&use"));
    connect(m_pauseAct, &QAction::triggered, this, &MainWindow::onPause);

    m_stepAct = toolbar->addAction(makeMediaIcon("step"), tr("&Step"));
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
    toolbar->addWidget(new QLabel(tr("  Color by: "), this));
    m_colorMode = new QComboBox(this);
    m_colorMode->addItem(tr("None"));
    m_colorMode->addItem(tr("Mean speed"));
    m_colorMode->addItem(tr("Occupancy"));
    m_colorMode->addItem(tr("Halting count"));
    toolbar->addWidget(m_colorMode);
    connect(m_colorMode,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int idx) {
        QMetaObject::invokeMethod(
            m_sim, "setColorMode", Qt::QueuedConnection, Q_ARG(int, idx));
    });

    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(tr("  Vehicles: "), this));
    auto* shapeCombo = new QComboBox(this);
    shapeCombo->addItem(tr("Rectangle"));
    shapeCombo->addItem(tr("Triangle"));
    shapeCombo->addItem(tr("Car"));
    shapeCombo->addItem(tr("Circle"));
    shapeCombo->setCurrentIndex(2);  // Car looks best by default
    toolbar->addWidget(shapeCombo);
    connect(shapeCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            m_view,
            &NetworkView::setVehicleShape);
    // Set the initial shape so the layer doesn't have to fall back to its
    // own default before the user touches the combo.
    QMetaObject::invokeMethod(m_view, "setVehicleShape", Qt::QueuedConnection,
                              Q_ARG(int, shapeCombo->currentIndex()));

    auto* vehColor = new QComboBox(this);
    vehColor->addItem(tr("by type"));
    vehColor->addItem(tr("by speed"));
    vehColor->addItem(tr("by waiting time"));
    vehColor->addItem(tr("by CO2"));
    vehColor->addItem(tr("by fuel"));
    toolbar->addWidget(vehColor);
    connect(vehColor,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int idx) {
        QMetaObject::invokeMethod(
            m_sim, "setVehicleColorMode", Qt::QueuedConnection, Q_ARG(int, idx));
    });

    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(tr("  Max fps: "), this));
    auto* fpsCombo = new QComboBox(this);
    fpsCombo->addItem(tr("uncapped"), 0);
    fpsCombo->addItem(tr("10"),  10);
    fpsCombo->addItem(tr("20"),  20);
    fpsCombo->addItem(tr("30"),  30);
    fpsCombo->addItem(tr("60"),  60);
    fpsCombo->addItem(tr("120"), 120);
    fpsCombo->setToolTip(tr(
        "Cap the snapshot-driven render refresh rate. Mirrors ecal_deck's "
        "MAX_PUBLISH_FPS so the qt_cpp vs ecal_deck perf comparison can be "
        "pinned to the same frame budget. User pan/zoom is never throttled."));
    toolbar->addWidget(fpsCombo);
    connect(fpsCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this, fpsCombo](int idx) {
        const int fps = fpsCombo->itemData(idx).toInt();
        m_view->setMaxFps(fps);
    });

    toolbar->addSeparator();
    toolbar->addAction(resetAct);

    // Per-layer visibility toggles. Each checkable QAction drives both the
    // GPU draw (NetworkView) and the per-layer Batch::fill* gate in the
    // worker, so hiding a layer also stops paying its extraction cost.
    auto addLayerToggle = [&](const QString& label,
                              const char* viewSlot, const char* simSlot) {
        auto* act = toolbar->addAction(label);
        act->setCheckable(true);
        act->setChecked(true);
        connect(act, &QAction::toggled, this, [this, viewSlot, simSlot](bool on) {
            QMetaObject::invokeMethod(m_view, viewSlot, Qt::DirectConnection, Q_ARG(bool, on));
            QMetaObject::invokeMethod(m_sim,  simSlot,  Qt::QueuedConnection, Q_ARG(bool, on));
        });
    };
    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(tr("  Show: "), this));
    addLayerToggle(tr("Vehicles"), "setVehiclesVisible", "setVehiclesVisible");
    addLayerToggle(tr("Persons"),  "setAgentsVisible",   "setAgentsVisible");
    addLayerToggle(tr("Signals & stop lines"), "setTLSVisible", "setTLSVisible");
    addLayerToggle(tr("EdgeData"), "setEdgeDataVisible", "setEdgeDataVisible");

    toolbar->addSeparator();
    auto* followAct = toolbar->addAction(tr("Follow selected"));
    followAct->setShortcut(QKeySequence(tr("Ctrl+F")));
    followAct->setToolTip(tr("Lock the camera onto the currently picked vehicle (Ctrl+F)"));
    connect(followAct, &QAction::triggered, m_view, &NetworkView::setFollowSelected);
    auto* unfollowAct = toolbar->addAction(tr("Unfollow"));
    unfollowAct->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(unfollowAct, &QAction::triggered, m_view, &NetworkView::clearFollow);
}

void MainWindow::buildStatusBar() {
    m_status = new QLabel(tr("Ready."), this);
    statusBar()->addWidget(m_status, 1);
    m_cursorLbl = new QLabel(tr("—"), this);
    m_cursorLbl->setToolTip(tr("Cursor world coordinates (SUMO XY in meters)"));
    statusBar()->addPermanentWidget(m_cursorLbl);
    m_benchLbl = new QLabel(tr("—"), this);
    m_benchLbl->setToolTip(tr("Rolling steps/s, snapshots/s, skip rate, avg "
                              "wall-time per sim step and per snapshot build"));
    statusBar()->addPermanentWidget(m_benchLbl);
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

void MainWindow::onBenchmark(double stepsPerSec, double snapshotsPerSec,
                             double skipRate, double avgStepMs,
                             double avgBuildMs) {
    m_benchLbl->setText(tr("%1 step/s  %2 snap/s  skip %3%  step %4ms  build %5ms")
                            .arg(stepsPerSec,    0, 'f', 1)
                            .arg(snapshotsPerSec, 0, 'f', 1)
                            .arg(skipRate * 100, 0, 'f', 1)
                            .arg(avgStepMs,      0, 'f', 2)
                            .arg(avgBuildMs,     0, 'f', 2));
}

void MainWindow::onCursor(double x, double y) {
    m_cursorLbl->setText(tr("xy: %1, %2 m").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1));
}

void MainWindow::onLog(int level, const QString& text) {
    static const char* const kLabel[3] = {"INFO", "WARN", "ERR "};
    static const char* const kColor[3] = {"#cccccc", "#e0b040", "#e05050"};
    const int lvl = (level < 0 || level > 2) ? 0 : level;
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz"));
    // HTML so we can colour the level tag without staining the message.
    const QString html = QStringLiteral(
        "<span style=\"color:#888\">%1</span> "
        "<span style=\"color:%2;font-weight:bold\">%3</span> "
        "<span style=\"color:%4\">%5</span>")
            .arg(ts, kColor[lvl], QString::fromUtf8(kLabel[lvl]),
                 kColor[lvl], text.toHtmlEscaped());
    m_logPanel->appendHtml(html);
}

// ---- window visibility -> worker extraction gating ------------------------
// Tells the worker to skip Batch::fill* when nothing on screen needs them.
// Mirrors sumo-gui only asking libsumo for what it draws.
void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    if (m_sim) QMetaObject::invokeMethod(
        m_sim, "setWindowVisible", Qt::QueuedConnection, Q_ARG(bool, true));
}

void MainWindow::hideEvent(QHideEvent* e) {
    QMainWindow::hideEvent(e);
    if (m_sim) QMetaObject::invokeMethod(
        m_sim, "setWindowVisible", Qt::QueuedConnection, Q_ARG(bool, false));
}

void MainWindow::changeEvent(QEvent* e) {
    QMainWindow::changeEvent(e);
    if (e->type() == QEvent::WindowStateChange && m_sim) {
        const bool visible = !(windowState() & Qt::WindowMinimized);
        QMetaObject::invokeMethod(
            m_sim, "setWindowVisible", Qt::QueuedConnection,
            Q_ARG(bool, visible));
    }
}
