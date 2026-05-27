#include <QApplication>
#include <QCommandLineParser>
#include <clocale>

#include "MainWindow.h"

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
        "Native Qt6 + OpenGL viewer for SUMO simulations (libsumo-coupled).");
    parser.addHelpOption();
    QCommandLineOption sumocfgOpt(
        QStringList{"s", "sumocfg"},
        "Path to a .sumocfg file to open on startup.",
        "path");
    parser.addOption(sumocfgOpt);
    parser.process(app);

    MainWindow win;
    win.resize(1280, 800);
    win.show();

    if (parser.isSet(sumocfgOpt)) {
        win.loadSumocfg(parser.value(sumocfgOpt));
    }

    return app.exec();
}
