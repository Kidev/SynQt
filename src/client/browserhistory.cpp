// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "browserhistory.h"

#include <QLoggingCategory>
#include <QPointer>

#include <utility>

#ifdef Q_OS_WASM
#include <emscripten.h>
#include <emscripten/val.h>
#endif

namespace SynQt {

namespace {

/// The live instance, for the popstate trampoline. One client has one history.
QPointer<BrowserHistory> s_instance;

QString normalizedBase(QString base)
{
    if (!base.startsWith(QLatin1Char('/'))) {
        base.prepend(QLatin1Char('/'));
    }
    while (base.endsWith(QLatin1Char('/')) && base.size() > 1) {
        base.chop(1);
    }
    return base;
}

} // namespace

#ifdef Q_OS_WASM

// Called from the popstate listener installed below; kept through the link because no C++
// references it.
extern "C" EMSCRIPTEN_KEEPALIVE void synqt_browserhistory_popped(const char *path)
{
    if (s_instance) {
        // Called directly, not queued. A queued call posts a QEvent::MetaCall, and on this
        // platform one lost browser callback disables posted-event delivery for the life of
        // the page (see tests/m0-transport/FIREFOX-LINUX.md), which would silently break
        // Back and Forward. Non-asyncify WebAssembly delivers every Qt event this way
        // (Module.qtSendPendingEvents() calls the handlers from the browser callback).
        // QString::fromUtf8 copies, so the caller may free the buffer on return.
        s_instance->handlePopped(QString::fromUtf8(path));
    }
}

// Emscripten prunes JS runtime helpers nothing declares, and does not scan EM_JS bodies.
// This body runs only on a real Back or Forward, so declaring the helpers turns a missing
// one into a link error. $stringToNewUTF8 brings in malloc; _free is named because the body
// calls it.
EM_JS_DEPS(synqt_browserhistory, "$stringToNewUTF8,free");

EM_JS(void, synqt_install_popstate_listener, (), {
    if (Module.synqtPopstateInstalled) {
        return;
    }
    Module.synqtPopstateInstalled = true;
    window.addEventListener("popstate", function () {
        var path = window.location.pathname + window.location.search;
        var buffer = stringToNewUTF8(path);
        Module._synqt_browserhistory_popped(buffer);
        _free(buffer);
    });
});

#endif // Q_OS_WASM

bool leaveForUrl(const QString &url)
{
    if (url.isEmpty()) {
        return false;
    }
#ifdef Q_OS_WASM
    // assign() rather than href=: both navigate, and assign states it. The browser tears
    // the app down and rebuilds it when the edge redirects back.
    emscripten::val::global("location").call<void>("assign", url.toStdString());
    return true;
#else
    // A desktop build has no page to leave. Sign-in there opens the system browser and
    // waits on a loopback port (SynClient::beginDesktopLogin); nothing else asks a native
    // window to navigate.
    qWarning("SynQt: %s is a browser navigation, and this is a native build, which has no "
             "page to leave. See https://synqt.org/desktop/.",
             qUtf8Printable(url));
    return false;
#endif
}

BrowserHistory::BrowserHistory(QString basePath, QObject *parent)
    : QObject{parent}
    , m_base{normalizedBase(std::move(basePath))}
{
    s_instance = this;
#ifdef Q_OS_WASM
    synqt_install_popstate_listener();
#else
    m_stack.append(QStringLiteral("/"));
#endif
}

BrowserHistory::~BrowserHistory() = default;

QString BrowserHistory::currentPath() const
{
#ifdef Q_OS_WASM
    const emscripten::val location{emscripten::val::global("location")};
    const QString path{
        QString::fromStdString(location["pathname"].as<std::string>())
        + QString::fromStdString(location["search"].as<std::string>())};
    return toApplicationPath(path);
#else
    return m_stack.value(m_index, QStringLiteral("/"));
#endif
}

void BrowserHistory::push(const QString &path)
{
#ifdef Q_OS_WASM
    emscripten::val::global("history").call<void>(
        "pushState", emscripten::val::null(), std::string{},
        toBrowserPath(path).toStdString());
#else
    m_stack.remove(m_index + 1, m_stack.size() - m_index - 1);
    m_stack.append(path);
    m_index = static_cast<int>(m_stack.size()) - 1;
#endif
}

void BrowserHistory::replace(const QString &path)
{
#ifdef Q_OS_WASM
    emscripten::val::global("history").call<void>(
        "replaceState", emscripten::val::null(), std::string{},
        toBrowserPath(path).toStdString());
#else
    if (m_stack.isEmpty()) {
        m_stack.append(path);
        m_index = 0;
        return;
    }
    m_stack[m_index] = path;
#endif
}

void BrowserHistory::back()
{
#ifdef Q_OS_WASM
    emscripten::val::global("history").call<void>("back");
#else
    if (m_index > 0) {
        --m_index;
        notifyPopped(m_stack.at(m_index));
    }
#endif
}

void BrowserHistory::forward()
{
#ifdef Q_OS_WASM
    emscripten::val::global("history").call<void>("forward");
#else
    if (m_index + 1 < m_stack.size()) {
        ++m_index;
        notifyPopped(m_stack.at(m_index));
    }
#endif
}

QString BrowserHistory::toBrowserPath(const QString &applicationPath) const
{
    if (m_base == QLatin1String("/")) {
        return applicationPath;
    }
    return m_base + applicationPath;
}

QString BrowserHistory::toApplicationPath(const QString &browserPath) const
{
    if (m_base == QLatin1String("/")) {
        return browserPath.isEmpty() ? QStringLiteral("/") : browserPath;
    }
    if (browserPath == m_base) {
        return QStringLiteral("/");
    }
    if (browserPath.startsWith(m_base + QLatin1Char('/'))) {
        return browserPath.mid(m_base.size());
    }
    return browserPath;
}

void BrowserHistory::handlePopped(const QString &path)
{
    emit popped(toApplicationPath(path));
}

void BrowserHistory::notifyPopped(const QString &path)
{
    emit popped(path);
}

} // namespace SynQt
