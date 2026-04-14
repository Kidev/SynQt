// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "api.h"

#include "apirequest.h"

#include <QJSEngine>
#include <QLoggingCategory>
#include <QQmlEngine>

#include <algorithm>

namespace SynQt {

Api::Api(QJSEngine *engine, QObject *parent)
    : QObject{parent}
    , m_engine{engine}
{
}

void Api::get(const QString &path, const QJSValue &handler)
{
    add(QStringLiteral("GET"), path, handler);
}

void Api::post(const QString &path, const QJSValue &handler)
{
    add(QStringLiteral("POST"), path, handler);
}

void Api::put(const QString &path, const QJSValue &handler)
{
    add(QStringLiteral("PUT"), path, handler);
}

void Api::del(const QString &path, const QJSValue &handler)
{
    add(QStringLiteral("DELETE"), path, handler);
}

void Api::route(const QString &method, const QString &path, const QJSValue &handler)
{
    add(method.toUpper(), path, handler);
}

QList<Api::Route> Api::routes() const
{
    return m_routes;
}

void Api::setListening(bool listening)
{
    m_listening = listening;
}

void Api::add(const QString &method, const QString &path, const QJSValue &handler)
{
    const RoutePattern pattern{path};
    if (!pattern.isValid()) {
        qWarning("SynQt: Api.%s(\"%s\", ...) is not a valid route path and was not added; "
                 "a path is absolute and its placeholders are named, as in /lots/:id",
                 qUtf8Printable(method.toLower()), qUtf8Printable(path));
        return;
    }
    if (!handler.isCallable()) {
        qWarning("SynQt: Api.%s(\"%s\", ...) was given something that is not a function; "
                 "the route was not added",
                 qUtf8Printable(method.toLower()), qUtf8Printable(path));
        return;
    }
    for (const Route &existing : m_routes) {
        if (existing.method == method && existing.pattern.pattern() == pattern.pattern()) {
            qWarning("SynQt: %s %s is declared twice; the first handler is kept",
                     qUtf8Printable(method), qUtf8Printable(path));
            return;
        }
    }

    m_routes.append(Route{method, pattern, handler});
    // Most literal segments first, so /lots/open beats /lots/:id in any declaration order.
    // std::stable_sort keeps equally specific patterns in declaration order.
    std::stable_sort(m_routes.begin(), m_routes.end(), [](const Route &a, const Route &b) {
        return a.pattern.literalSegmentCount() > b.pattern.literalSegmentCount();
    });
    if (m_listening) {
        emit routeAddedLate(method, path);
    }
}

bool Api::dispatch(ApiRequest *request) const
{
    // Split once for the whole table, not once per route. A path that cannot be split
    // matches no pattern.
    QStringList segments;
    if (!RoutePattern::splitPath(request->path(), &segments)) {
        return false;
    }
    for (const Route &route : m_routes) {
        if (route.method != request->method()) {
            continue;
        }
        QVariantMap parameters;
        if (!route.pattern.matches(segments, &parameters)) {
            continue;
        }

        // The request carries this route's captures and is the handler's only argument.
        // Owned by C++, since the handler may keep it to answer later; the server parents
        // it and retires it with the response.
        request->setParams(parameters);
        QQmlEngine::setObjectOwnership(request, QQmlEngine::CppOwnership);
        QJSValue handler{route.handler};
        const QJSValue result{handler.call(QJSValueList{m_engine->newQObject(request)})};
        if (result.isError()) {
            qWarning("SynQt: the handler for %s %s threw: %s",
                     qUtf8Printable(route.method), qUtf8Printable(route.pattern.pattern()),
                     qUtf8Printable(result.toString()));
            request->fail(500, QStringLiteral("handler error"));
            return true;
        }
        // A returned value from a handler that has not answered is the answer, so a
        // synchronous handler is one line. A handler that already answered, or returned
        // nothing to answer later, is left alone.
        if (!request->isAnswered() && !result.isUndefined() && !result.isNull()) {
            request->reply(result.toVariant());
        }
        return true;
    }
    return false;
}

} // namespace SynQt
