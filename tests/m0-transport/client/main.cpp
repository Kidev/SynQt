// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "m0controller.h"

#include <QByteArray>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QString>
#include <QUrl>
#include <QtGlobal>

#include <cstdio>

#ifdef Q_OS_WASM
#include <emscripten/console.h>
#include <emscripten/emscripten.h>
#endif

namespace {

// The b70743f run proved the serial-id diagnostic needs its own console path: enabling
// qt.remoteobjects.debug printed nothing at all, on every browser, because WASM routes
// qInfo/qWarning to the browser console but drops category qCDebug at debug level, so QtRO's
// "serial id" lines never reached the CI log. Install a message handler that forwards through
// emscripten_console_log (a direct JS console.log the verify harness captures) instead of the
// default WASM handler. Keep the log focused and the single-threaded event loop unperturbed:
// forward our own "M0 " markers and every non-debug message verbatim, and of QtRO's debug
// flood forward only the two lines that carry the reply serial, tagged "M0 QTRO " so they sit
// beside the existing "M0 echo sent" / "M0 rx frame bytes" markers and stay greppable.
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
    char *raw{emscripten_run_script_string(
        "(function(){return (new URLSearchParams(window.location.search))"
        ".get('url') || '';})()")};
    url = QString::fromUtf8(raw);
#endif
    if (url.isEmpty()) {
        url = qEnvironmentVariable("M0_URL", QStringLiteral("ws://localhost:8088"));
    }
    return QUrl{url};
}

} // namespace

int main(int argc, char *argv[])
{
    // Diagnostic for the firefox-on-CI reply=false split (see tests/m0-transport/README.md
    // and the frame-size instrument): the frame-size logging already proved the 69-byte
    // reply frame ARRIVES at the client, yet the returning-slot PendingCall never resolves
    // (watcher `finished` never fires, no error). The next question is where inside QtRO it
    // dies, and QtRO answers it itself at debug level (with no rebuild of the kit) through
    // two lines under the qt.remoteobjects category that carry the serial the client SENT and
    // the serial each reply ACKs:
    //   "Sent InvokePacket with serial id: N"                         (uplink invoke)
    //   "<name> Received InvokeReplyPacket ack'ing serial id: M"      (the reply dispatched)
    // These land in the CI log (tagged "M0 QTRO ...") next to the "M0 echo sent" / "M0 rx frame
    // bytes=69" markers, but ONLY via the custom handler below: the b70743f run proved that
    // enabling the category alone prints nothing on WASM, because the platform drops category
    // qCDebug at debug level while carrying qInfo/qWarning. The handler re-routes through
    // emscripten_console_log so the lines actually reach the harness. Reading rule for the next
    // run, in the failing firefox-ws window:
    //   * no "Received InvokeReplyPacket" line despite an rx=69 frame -> the read loop never
    //     classifies the frame as a reply (framing/dispatch miss; enable .io next);
    //   * M == 0 -> the reply decodes to the heartbeat serial and is dropped by the
    //     ackedSerialId==0 branch of notifyAboutReply (serial corruption to 0);
    //   * M != any sent N -> serial mismatch/corruption on the wire;
    //   * M == a sent N -> it is in m_pendingCalls and the watcher/emit path is the culprit.
    // The .io category (per-read framing) is intentionally left off here to keep perturbation low.
    qInstallMessageHandler(m0MessageHandler);
    QLoggingCategory::setFilterRules(QStringLiteral(
        "qt.remoteobjects.debug=true\nqt.remoteobjects.warning=true"));
    QGuiApplication app{argc, argv};

    const QUrl edgeUrl{resolveEdgeUrl()};
    qInfo().noquote()
        << QStringLiteral("M0 client starting url=%1").arg(edgeUrl.toString());

    M0Controller *controller{new M0Controller{edgeUrl, &app}};

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("m0"), controller);
    engine.loadFromModule("M0Client", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }
    return app.exec();
}
