// Minimal driver for the QRhi vehicle-layer spike. Loads the sumocfg passed
// on the command line, runs SimWorker on a background thread, displays only
// vehicles via VehicleLayerRhi on a QRhiWidget.

#include <QApplication>
#include <QThread>
#include <QCommandLineParser>
#include <Qt>
#include <clocale>

#include "RhiSpikeWidget.h"
#include "sim/SimWorker.h"

int main(int argc, char** argv) {
    // Fractional DPR (e.g. 1.08x on X11) breaks our QWindow+createWindowContainer
    // swap-chain sizing: surfacePixelSize comes back DPR-scaled but the GLX
    // drawable stays at logical pixels, so the rendered content gets clipped
    // to the lower-left of the window. Round DPR to integer (1.0 here) before
    // QApplication is constructed.
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::Floor);

    QApplication app(argc, argv);
    app.setApplicationName("qt_cpp_rhi_spike");

    // QApplication's constructor calls setlocale(LC_ALL,"") which under
    // comma-decimal locales (e.g. de_DE) breaks libsumo's std::stod-based
    // XML parsing. Force C numeric formatting before any libsumo call.
    std::setlocale(LC_NUMERIC, "C");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "QRhi vehicle-layer spike (Qt RHI feasibility test).");
    parser.addPositionalArgument("sumocfg", "Path to .sumocfg to load.");
    parser.addHelpOption();
    parser.process(app);

    const auto args = parser.positionalArguments();
    if (args.isEmpty()) {
        qWarning("usage: qt_cpp_rhi_spike <path/to/scenario.sumocfg>");
        return 2;
    }
    const QString sumocfg = args.first();

    RhiSpikeWidget view;
    view.resize(1280, 800);
    view.setWindowTitle("Qt RHI vehicle layer spike");
    view.show();

    QThread simThread;
    SimWorker worker;
    worker.moveToThread(&simThread);
    QObject::connect(&simThread, &QThread::finished,
                     &worker,    &SimWorker::shutdown);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
        simThread.quit();
        simThread.wait(2000);
    });
    view.attachWorker(&worker);
    QObject::connect(&worker, &SimWorker::errorOccurred,
                     [](const QString& msg) {
        qWarning("SimWorker error: %s", qPrintable(msg));
    });

    simThread.start();

    // Kick off scenario load + autoplay once the thread is up.
    QMetaObject::invokeMethod(&worker, "loadScenario",
                              Qt::QueuedConnection,
                              Q_ARG(QString, sumocfg));
    QMetaObject::invokeMethod(&worker, "play", Qt::QueuedConnection);

    return app.exec();
}
