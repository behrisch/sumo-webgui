#pragma once

// LogCapture — bridges libsumo's static MsgHandler streams into a Qt signal
// so the GUI can show a dockable log panel.  We subclass libsumo's
// OutputDevice (it lives in SUMO's util/iodevices library, which is folded
// into libsumocpp.so on Linux — see qt_cpp/PLAN.md "Diagnostics").
//
// OutputDevice.h depends on SUMO's autoconf-generated <config.h>, which
// isn't part of the public libsumo include set.  To avoid forcing every
// translation unit that touches the log path to find config.h, the
// OutputDevice subclass lives entirely inside LogCapture.cpp; this header
// exposes only the QObject "router" (so moc can see it) and an opaque
// LogCaptureSet handle.

#include <QObject>
#include <QString>
#include <memory>

class LogRouter : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
signals:
    // level: 0=info, 1=warning, 2=error.  Emitted from the sim thread;
    // queued-connected to MainWindow on the GUI thread.
    void logged(int level, QString text);
};

// Opaque holder for the three OutputDevice subclasses we register with
// MsgHandler.  Destructor removes them from MsgHandler before they die.
struct LogCaptureSet {
    LogCaptureSet();
    ~LogCaptureSet();
    LogCaptureSet(LogCaptureSet&&) noexcept;
    LogCaptureSet& operator=(LogCaptureSet&&) noexcept;
    LogCaptureSet(const LogCaptureSet&) = delete;
    LogCaptureSet& operator=(const LogCaptureSet&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Create + register three captures (Info/Warning/Error) on libsumo's
// process-global MsgHandler.  Must be called from the same thread that will
// later call Simulation::start (the captures are read without locking).
LogCaptureSet installLogCaptures(LogRouter* router);
