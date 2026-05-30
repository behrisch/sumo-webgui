#include "MainWindow.h"

#include <QAction>
#include <QComboBox>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QIcon>
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
    toolbar->addAction(resetAct);

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
