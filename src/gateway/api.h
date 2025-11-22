// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_API_H
#define SYNQT_API_H

#include "routepattern.h"

#include <QJSValue>
#include <QList>
#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QJSEngine;
QT_END_NAMESPACE

namespace SynQt {

class ApiRequest;

/// The inbound HTTP helper, exposed as `Api` to the QML of an entity whose
/// `network.inbound` says it serves one. The mirror of `Http`: one declares where this entity
/// may call, the other who may call it and what they may ask for.
///
/// Routes are declared from QML, in the entity's own singleton. The topology says who may
/// reach this entity and on which port; the routes are the entity's own, like its
/// connect-point slots.
///
///     Api.get("/lots/:id", request => {
///         Books.ledger.lot(request.params.id)
///             .then(lot => request.reply(lot),
///                   error => request.fail(404, error));
///     });
///
/// A handler that returns a value without having answered replies with it as 200. A handler
/// that answers later returns nothing.
class Api : public QObject
{
    Q_OBJECT

public:
    explicit Api(QJSEngine *engine, QObject *parent = nullptr);

    Q_INVOKABLE void get(const QString &path, const QJSValue &handler);
    Q_INVOKABLE void post(const QString &path, const QJSValue &handler);
    Q_INVOKABLE void put(const QString &path, const QJSValue &handler);
    Q_INVOKABLE void del(const QString &path, const QJSValue &handler);
    /// Any method, for the ones the four above do not name (PATCH, HEAD).
    Q_INVOKABLE void route(const QString &method, const QString &path,
                           const QJSValue &handler);

    /// Every declared route, in the order the server should try them: most literal
    /// segments first, so `/lots/open` wins over `/lots/:id` whichever was declared
    /// first. Ordering by declaration would make the surface depend on the order QML
    /// happened to run in.
    struct Route
    {
        QString method;
        RoutePattern pattern;
        QJSValue handler;
    };
    QList<Route> routes() const;

    /// Run the handler for `request`, or return false when no route matches. The server
    /// turns false into a 404; nothing else in this class knows about HTTP.
    bool dispatch(ApiRequest *request) const;

signals:
    /// A route was declared after the server started listening. Reported rather than
    /// silently accepted. The surface a deployment reviewed is the one declared at
    /// startup, and a route that appears later is a surprise worth seeing in the log.
    void routeAddedLate(const QString &method, const QString &path);

public:
    /// Called by the server once it is listening, so `routeAddedLate` can mean something.
    void setListening(bool listening);

private:
    void add(const QString &method, const QString &path, const QJSValue &handler);

    QJSEngine *m_engine;
    QList<Route> m_routes;
    bool m_listening{false};
};

} // namespace SynQt

#endif // SYNQT_API_H
