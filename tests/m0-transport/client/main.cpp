// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "m0controller.h"

#include <QByteArray>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QtGlobal>

#include <cstdio>

#ifdef Q_OS_WASM
#include <emscripten/bind.h>
#include <emscripten/console.h>
#include <emscripten/emscripten.h>
#include <emscripten/val.h>

#include <string>
#endif

namespace {

// WASM routes qInfo/qWarning to the browser console but drops category qCDebug at debug
// level, so enabling qt.remoteobjects.debug alone prints nothing. This message handler
// forwards through emscripten_console_log (a direct JS console.log the verify harness
// captures) instead of the default WASM handler. To keep the log focused and the
// single-threaded event loop unperturbed, it forwards the spike's own "M0 " markers and
// every non-debug message verbatim, and from QtRO's debug output only the two lines that
// carry the reply serial, tagged "M0 QTRO " so they sit beside the "M0 echo sent" / "M0 rx
// frame bytes" markers.
void m0MessageHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    const bool isSerialTrace{msg.contains(QLatin1String("serial id"))};
    const bool isOurMarker{msg.startsWith(QLatin1String("M0 "))};
    if (type == QtDebugMsg && !isSerialTrace && !isOurMarker) {
        return;
    }
    const QByteArray line{isSerialTrace
                              ? QByteArray{"M0 QTRO "} + msg.toUtf8()
                              : msg.toUtf8()};
#ifdef Q_OS_WASM
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        emscripten_console_error(line.constData());
    } else {
        emscripten_console_log(line.constData());
    }
#else
    std::fprintf(stderr, "%s\n", line.constData());
    std::fflush(stderr);
#endif
}

// The edge URL is passed as ?url=<scheme>://host:port on the page that hosts the
// bundle, so one build tests both ws and wss. On the desktop it can come from the
// M0_URL environment variable instead. Falls back to the plaintext dev port.
QUrl resolveEdgeUrl()
{
    QString url;
#ifdef Q_OS_WASM
    // Through Embind, not emscripten_run_script_string. That one is an eval, so it needs
    // `script-src 'unsafe-eval'` and the CSP SynQt's edge emits does not grant it.
    // The product reads the browser the same way for the same reason (BrowserHistory, and
    // the -sDYNAMIC_EXECUTION=0 the generated client links with), and this spike is only
    // worth anything if it runs the way the shipped client does.
    const emscripten::val params{emscripten::val::global("URLSearchParams").new_(
        emscripten::val::global("location")["search"])};
    const emscripten::val value{params.call<emscripten::val>("get", std::string{"url"})};
    if (!value.isNull() && !value.isUndefined()) {
        url = QString::fromStdString(value.as<std::string>());
    }
#endif
    if (url.isEmpty()) {
        url = qEnvironmentVariable("M0_URL", QStringLiteral("ws://localhost:8088"));
    }
    return QUrl{url};
}

} // namespace

int main(int argc, char *argv[])
{
    // Diagnostic for the Firefox-on-CI reply path (see tests/m0-transport/README.md and the
    // frame-size instrument), where the reply frame arrives at the client and the
    // returning-slot PendingCall never resolves. QtRO reports where a reply goes through two
    // lines under the qt.remoteobjects category, carrying the serial the client SENT and the
    // serial each reply ACKs:
    //   "Sent InvokePacket with serial id: N"                         (uplink invoke)
    //   "<name> Received InvokeReplyPacket ack'ing serial id: M"      (the reply dispatched)
    // They reach the log (tagged "M0 QTRO ...") only through the handler above. Reading them
    // in a failing window:
    //   * no "Received InvokeReplyPacket" line despite an rx frame -> the read loop never
    //     classifies the frame as a reply (framing or dispatch; enable .io next);
    //   * M == 0 -> the reply decodes to the heartbeat serial and is dropped by the
    //     ackedSerialId==0 branch of notifyAboutReply;
    //   * M != any sent N -> serial mismatch or corruption on the wire;
    //   * M == a sent N -> it is in m_pendingCalls and the watcher/emit path is at fault.
    // The .io category (per-read framing) stays off to keep perturbation low.
    qInstallMessageHandler(m0MessageHandler);
    QLoggingCategory::setFilterRules(QStringLiteral(
        "qt.remoteobjects.debug=true\nqt.remoteobjects.warning=true"));
    QGuiApplication app{argc, argv};

    const QUrl edgeUrl{resolveEdgeUrl()};
    qInfo().noquote()
        << QStringLiteral("M0 client starting url=%1").arg(edgeUrl.toString());

#if defined(M0_POSTED_EVENT_PUMP)
    // The application-side answer to the defect the Qt patch in qt-patches/ addresses, so
    // the two can be measured against each other. Qt for WebAssembly delivers posted events
    // from one chain of zero-delay browser callbacks, armed by wakeUp() when an event is
    // posted and never re-armed while it waits, so a callback the browser drops takes the
    // event with it. A QTimer is delivered by a different mechanism
    // (QTimerInfoList::activateTimers uses sendEvent), so it still arrives, and sweeping
    // the posted queue from one gives delivery a second path.
    //
    // Unfiltered. A filtered sweep that passes only QEvent::MetaCall breaks reconnect on
    // Firefox: draining one event type from a queue that holds several reorders them, and
    // DeferredDelete in particular has to stay where it is. Unfiltered, this is the same
    // sweep QEventDispatcherWasm itself performs.
    QTimer *postedEventPump{new QTimer{&app}};
    postedEventPump->setInterval(50);
    QObject::connect(postedEventPump, &QTimer::timeout, &app, []() {
        QCoreApplication::sendPostedEvents();
    });
    postedEventPump->start();
#endif

    M0Controller *controller{new M0Controller{edgeUrl, &app}};

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("m0"), controller);
    engine.loadFromModule("M0Client", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }
    return app.exec();
}
