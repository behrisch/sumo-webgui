#include <QApplication>
#include <QCommandLineParser>
#include <QObject>
#include <clocale>
#include <cstdio>

#include "MainWindow.h"
#include "sim/SimWorker.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("qt_cpp");
    QApplication::setApplicationDisplayName("SUMO Qt6 GUI");

    // QApplication's constructor calls setlocale(LC_ALL, "") which picks up
    // the user's locale (e.g. de_DE). libsumo parses numeric option values
    // and XML floats with std::stod, which is locale-sensitive and would
    // reject "0.2" under a comma-decimal locale. Force C numeric formatting
    // for all SUMO interactions. Qt UI strings still respect QLocale.
    std::setlocale(LC_NUMERIC, "C");

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Native Qt6 + RHI viewer for SUMO simulations (libsumo-coupled).");
    parser.addHelpOption();
    QCommandLineOption sumocfgOpt(
        QStringList{"s", "sumocfg"},
        "Path to a .sumocfg file to open on startup.",
        "path");
    parser.addOption(sumocfgOpt);
    QCommandLineOption benchOpt(
        QStringList{"b", "benchmark"},
        "Headless-ish benchmark mode: auto-load the .sumocfg, force delay=0, "
        "start playing immediately and exit with a one-line summary as soon as "
        "the simulation ends. Requires --sumocfg. The GUI window is still "
        "shown so the full render pipeline is exercised (matches "
        "ecal_deck's --benchmark-full behaviour rather than --benchmark).");
    parser.addOption(benchOpt);
    parser.addPositionalArgument("sumocfg",
        "Path to a .sumocfg file to open on startup (positional alias for -s).",
        "[sumocfg]");
    parser.process(app);

    MainWindow win;
    win.resize(1280, 800);
    const bool benchmark = parser.isSet(benchOpt);
    if (benchmark) {
        // Show the window full-screen so the render workload matches the
        // ecal_deck side of the comparison (the browser is run F11 / full
        // viewport for benchmarks).
        win.showFullScreen();
    } else {
        win.show();
    }

    QString cfg;
    if (parser.isSet(sumocfgOpt)) {
        cfg = parser.value(sumocfgOpt);
    } else {
        const QStringList pos = parser.positionalArguments();
        if (!pos.isEmpty()) cfg = pos.first();
    }

    if (benchmark && cfg.isEmpty()) {
        std::fprintf(stderr,
            "qt_cpp: --benchmark requires a .sumocfg path (use -s PATH or a "
            "positional argument).\n");
        return 2;
    }

    if (!cfg.isEmpty()) {
        win.loadSumocfg(cfg);
    }

    if (benchmark) {
        SimWorker* sim = win.simWorker();
        // Force max-speed stepping and auto-start as soon as the scenario
        // finishes loading. scenarioLoaded fires on the sim thread; queued
        // connections marshal the play / setDelayMs calls onto the same.
        QObject::connect(sim, &SimWorker::scenarioLoaded, sim,
            [sim](const QString&) {
                QMetaObject::invokeMethod(sim, "setDelayMs",
                    Qt::QueuedConnection, Q_ARG(int, 0));
                QMetaObject::invokeMethod(sim, "play",
                    Qt::QueuedConnection);
            });
        // Whole-run summary, fired exactly once, then exit.
        QObject::connect(sim, &SimWorker::simulationEnded, &app,
            [&app](qint64 steps, double simTime, double wallSec,
                   double avgStepMs, double avgBuildMs,
                   double snapsPerSec, double skipRate) {
                std::fprintf(stderr,
                    "[benchmark] qt_cpp: steps=%lld sim_time=%.2fs "
                    "wall=%.2fs steps/s=%.1f snapshots/s=%.1f skip=%.1f%% "
                    "avg_step=%.3fms avg_build=%.3fms\n",
                    static_cast<long long>(steps), simTime, wallSec,
                    wallSec > 0 ? steps / wallSec : 0.0,
                    snapsPerSec, skipRate * 100.0,
                    avgStepMs, avgBuildMs);
                std::fflush(stderr);
                app.exit(0);
            });
    }

    return app.exec();
}
