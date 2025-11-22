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
    // Most literal segments first, so /lots/open beats /lots/:id whichever order the QML
    // declared them in. std::stable_sort, so two patterns of equal specificity keep their
    // declaration order and the table stays reproducible.
    std::stable_sort(m_routes.begin(), m_routes.end(), [](const Route &a, const Route &b) {
        return a.pattern.literalSegmentCount() > b.pattern.literalSegmentCount();
    });
    if (m_listening) {
        emit routeAddedLate(method, path);
    }
}

bool Api::dispatch(ApiRequest *request) const
{
    for (const Route &route : m_routes) {
        if (route.method != request->method()) {
            continue;
        }
        QVariantMap parameters;
        if (!route.pattern.matches(request->path(), &parameters)) {
            continue;
        }

        // The request carries this route's captures, then goes to the handler as its only
        // argument. Ownership stays in C++ because the handler may hold it past this call
        // to answer later. The server parents it and retires it with the response.
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
        // A handler that returned a value and has not answered meant that value as the
        // answer, which is what makes the synchronous case a one-liner. One that answered
        // already, or that returned nothing because it will answer later, is left alone.
        if (!request->isAnswered() && !result.isUndefined() && !result.isNull()) {
            request->reply(result.toVariant());
        }
        return true;
    }
    return false;
}

} // namespace SynQt
