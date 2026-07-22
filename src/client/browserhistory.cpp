// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "browserhistory.h"

#include <QMetaObject>
#include <QPointer>

#include <utility>

#ifdef Q_OS_WASM
#include <emscripten.h>
#include <emscripten/val.h>
#endif

namespace SynQt {

namespace {

/// The live instance, so the popstate trampoline can reach it. One client
/// runs one history; a second instance would mean a second router, which
/// the runtime never creates.
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

// Called from the popstate listener installed below. Kept alive through the
// link because nothing in C++ references it.
extern "C" EMSCRIPTEN_KEEPALIVE void synqt_browserhistory_popped(const char *path)
{
    if (s_instance) {
        // QMetaObject::invokeMethod is a static member; call it on the class,
        // not through an instance pointer, even though invoking it via
        // s_instance->metaObject()->invokeMethod(...) would also compile.
        QMetaObject::invokeMethod(
            s_instance, "handlePopped", Qt::QueuedConnection,
            Q_ARG(QString, QString::fromUtf8(path)));
    }
}

// Emscripten prunes any JS runtime helper nothing declares a need for, and
// it does not scan EM_JS bodies for the ones they call. The body below runs
// only on a real back or forward, so an undeclared helper would not show up
// until a visitor pressed Back. Declared here, a missing one is a link
// error instead. $stringToNewUTF8 pulls malloc in through its own
// dependency; free has to be named because the body calls _free directly.
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
