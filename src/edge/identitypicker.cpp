// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "identitypicker.h"

#include "sessionmanager.h"

#include <QDateTime>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QRandomGenerator>
#include <QUrlQuery>

namespace SynQt {

namespace {

/// The session table is full with nothing to drop: the same answer as every other minting
/// route, so the picker never hands out an empty cookie.
QHttpServerResponse tableFull()
{
    return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                               QByteArrayLiteral("no session can be issued right now"),
                               QHttpServerResponder::StatusCode::ServiceUnavailable};
}

/// The picker page: plain HTML without script or a styling framework, since the development
/// edge serves it under the project's own CSP, and a development tool must not require a
/// relaxed policy. One form per scope, so a choice is an ordinary POST.
QByteArray pageFor(const QStringList &scopeOrder,
                   const QList<QPair<QString, QString>> &named,
                   const QStringList &problems)
{
    QByteArray html{
        "<!doctype html>\n<html lang=\"en\">\n<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>SynQt development sign-in</title>\n"
        "</head>\n<body>\n"
        "<h1>Development sign-in</h1>\n"
        "<p>Pick a scope. This page exists only under "
        "<code>synqt dev --identity-picker</code>; it is not compiled into a build.</p>\n"};
    for (qsizetype index{0}; index < scopeOrder.size(); ++index) {
        const QString scope{scopeOrder.at(index)};
        html += "<form method=\"post\" action=\"" + IdentityPicker::route().toUtf8() + "\">\n"
                "<input type=\"hidden\" name=\"scope\" value=\""
                + QString::number(index).toUtf8() + "\">\n"
                // The per-tab checkbox sits inside each form, so the pressed button submits
                // the checkbox beside it; a single checkbox outside the forms would never
                // be submitted.
                "<label><input type=\"checkbox\" name=\"this_tab_only\" value=\"1\" "
                "id=\"this-tab-only-" + scope.toHtmlEscaped().toUtf8()
                + "\"> this tab only</label>\n"
                "<button type=\"submit\" data-scope=\"" + scope.toHtmlEscaped().toUtf8()
                + "\">" + scope.toHtmlEscaped().toUtf8() + "</button>\n"
                "</form>\n";
    }

    // The people from `.dev-identities`, if any. Listed second: picking a scope always
    // works, and this appears only when the file exists.
    if (!named.isEmpty()) {
        html += "<h2>Named identities</h2>\n"
                "<p>From <code>.dev-identities</code>. Each is a real address in a "
                "synthesized identity, so a project keyed to a person sees the same person "
                "every time.</p>\n";
        for (qsizetype index{0}; index < named.size(); ++index) {
            const QString email{named.at(index).first};
            const QString remark{named.at(index).second};
            html += "<form method=\"post\" action=\"" + IdentityPicker::route().toUtf8()
                    + "\">\n"
                      "<input type=\"hidden\" name=\"identity\" value=\""
                    + QString::number(index).toUtf8() + "\">\n"
                      "<label><input type=\"checkbox\" name=\"this_tab_only\" value=\"1\" "
                      "id=\"this-tab-only-identity-" + QString::number(index).toUtf8()
                    + "\"> this tab only</label>\n"
                      "<button type=\"submit\" data-identity=\"" + email.toHtmlEscaped().toUtf8()
                    + "\">" + email.toHtmlEscaped().toUtf8() + "</button>\n"
                      "<span data-remark>" + remark.toHtmlEscaped().toUtf8() + "</span>\n"
                      "</form>\n";
        }
    }

    // Entries from the file that could not be used, shown on the page where a missing name
    // is noticed.
    if (!problems.isEmpty()) {
        html += "<h2>Ignored entries</h2>\n<ul>\n";
        for (const QString &problem : problems) {
            html += "<li>" + problem.toHtmlEscaped().toUtf8() + "</li>\n";
        }
        html += "</ul>\n";
    }

    html += "</body>\n</html>\n";
    return html;
}

/// A name for one tab's cookie: random, so two developers on one edge do not collide, and
/// letters and digits only, since it goes into a cookie name (WebEdge validates the same
/// alphabet).
///
/// Not a credential: it selects the cookie, which holds the session id. Still generated
/// with QRandomGenerator::system(), so it is never predictable.
QByteArray freshNonce()
{
    QByteArray nonce;
    nonce.reserve(16);
    static const char kAlphabet[]{"abcdefghijklmnopqrstuvwxyz0123456789"};
    for (int index{0}; index < 16; ++index) {
        const quint32 pick{QRandomGenerator::system()->bounded(
            static_cast<quint32>(sizeof(kAlphabet) - 1))};
        nonce.append(kAlphabet[pick]);
    }
    return nonce;
}

}  // namespace

IdentityPicker::IdentityPicker(SessionManager *sessions, QStringList scopeOrder,
                               QObject *parent)
    : QObject{parent}
    , m_sessions{sessions}
    , m_scopeOrder{std::move(scopeOrder)}
{
}

void IdentityPicker::setNamedIdentities(const QList<WebEdgeConfig::DevIdentity> &identities,
                                        const QStringList &problems)
{
    m_named = identities;
    m_problems = problems;
}

void IdentityPicker::setScopeMapper(ScopeMapper mapper)
{
    m_mapper = std::move(mapper);
}

QString IdentityPicker::route()
{
    return QStringLiteral("/synqt/dev/identity");
}

QHttpServerResponse IdentityPicker::page() const
{
    // The hook is consulted while the page is drawn, so any disagreement between the file
    // and the hook is visible before choosing.
    QList<QPair<QString, QString>> named;
    named.reserve(m_named.size());
    for (const WebEdgeConfig::DevIdentity &identity : m_named) {
        named.append({identity.email, resolve(identity).remark});
    }
    return QHttpServerResponse{QByteArrayLiteral("text/html; charset=utf-8"),
                               pageFor(m_scopeOrder, named, m_problems)};
}

QVariantMap IdentityPicker::identityForNamed(const QString &email) const
{
    QVariantMap identity;
    // Stable across restarts, unlike the scope mode's timestamped `sub`, so a project that
    // stores data per person sees the same person next run. The prefix still keeps it from
    // colliding with a real provider id.
    identity.insert(QStringLiteral("sub"), QStringLiteral("synqt-dev:%1").arg(email));
    identity.insert(QStringLiteral("login"), email.section(QLatin1Char('@'), 0, 0));
    identity.insert(QStringLiteral("name"), email);
    identity.insert(QStringLiteral("email"), email);
    return identity;
}

IdentityPicker::Resolution IdentityPicker::resolve(
    const WebEdgeConfig::DevIdentity &identity) const
{
    if (!m_mapper) {
        // No identity provider on this edge, so no hook to ask; the page says so instead of
        // implying the hook agreed.
        return {identity.scope, QStringLiteral("%1 (from the file; this project has no "
                                               "mapping hook to ask)").arg(identity.scope)};
    }

    QString error;
    const QString mapped{m_mapper(identityForNamed(identity.email), &error)};
    if (mapped.isEmpty()) {
        // The hook refused this person, as a real login would. The picker refuses too, so
        // it never shows a state the application cannot reach.
        return {QString{}, QStringLiteral("refused by the mapping hook: %1").arg(error)};
    }
    if (mapped != identity.scope) {
        // They disagree: both are shown, and the session gets the hook's answer.
        return {mapped, QStringLiteral("%1 (the file says %2)").arg(mapped, identity.scope)};
    }
    return {mapped, mapped};
}

QVariantMap IdentityPicker::identityFor(const QString &scope) const
{
    QVariantMap identity;
    identity.insert(QStringLiteral("sub"),
                    QStringLiteral("synqt-dev:%1:%2")
                        .arg(scope)
                        .arg(QDateTime::currentMSecsSinceEpoch()));
    identity.insert(QStringLiteral("login"), scope);
    identity.insert(QStringLiteral("name"), QStringLiteral("Development %1").arg(scope));
    // Null, not absent or invented: `identity.email` is nullable for real providers too, so
    // a hook keyed on it behaves as it would when GitHub withholds it
    // (docs/authentication.md).
    identity.insert(QStringLiteral("email"), QVariant{});
    return identity;
}

QHttpServerResponse IdentityPicker::chooseNamed(const QString &picked,
                                                const QUrlQuery &form, Choice *choice)
{
    // An index into the list this page drew, bounds-checked like a posted scope: the list
    // came from a file.
    bool isNumber{false};
    const int index{picked.toInt(&isNumber)};
    if (!isNumber || index < 0 || index >= static_cast<int>(m_named.size())) {
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("not one of this project's named "
                                                     "development identities"),
                                   QHttpServerResponder::StatusCode::BadRequest};
    }

    const WebEdgeConfig::DevIdentity &identity{m_named.at(index)};
    const Resolution resolution{resolve(identity)};
    if (resolution.scope.isEmpty()) {
        // The project's hook refused this person, so the picker refuses too.
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   resolution.remark.toUtf8(),
                                   QHttpServerResponder::StatusCode::Forbidden};
    }
    // Bounds-checked here as well, in case a mapper other than the identity provider's is
    // ever used.
    if (!m_scopeOrder.contains(resolution.scope)) {
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("not one of this project's scopes"),
                                   QHttpServerResponder::StatusCode::BadRequest};
    }

    const QByteArray minted{m_sessions->createSession(resolution.scope,
                                                      identityForNamed(identity.email))};
    if (minted.isEmpty()) {
        return tableFull();
    }
    if (choice) {
        choice->sessionId = minted;
        if (!form.queryItemValue(QStringLiteral("this_tab_only")).isEmpty()) {
            choice->tabNonce = freshNonce();
        }
    }
    qInfo("SynQt: the development picker signed in '%s' as '%s'",
          qUtf8Printable(identity.email), qUtf8Printable(resolution.scope));
    return QHttpServerResponse{QByteArrayLiteral("text/plain"), QByteArrayLiteral("ok")};
}

QHttpServerResponse IdentityPicker::choose(const QHttpServerRequest &request,
                                           Choice *choice)
{
    const QUrlQuery form{QString::fromUtf8(request.body())};
    const QString named{form.queryItemValue(QStringLiteral("identity"), QUrl::FullyDecoded)};
    if (!named.isEmpty()) {
        return chooseNamed(named, form, choice);
    }
    const QString picked{form.queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded)};

    // An index into the declared vocabulary, bounds-checked like a mapping hook's answer.
    // The picker skips the hook, so this check alone stops a posted number from reaching an
    // undeclared scope.
    bool isNumber{false};
    const int index{picked.toInt(&isNumber)};
    if (!isNumber || index < 0 || index >= static_cast<int>(m_scopeOrder.size())) {
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("not one of this project's scopes"),
                                   QHttpServerResponder::StatusCode::BadRequest};
    }

    const QString scope{m_scopeOrder.at(index)};
    const QByteArray minted{m_sessions->createSession(scope, identityFor(scope))};
    if (minted.isEmpty()) {
        return tableFull();
    }
    if (choice) {
        choice->sessionId = minted;
        if (!form.queryItemValue(QStringLiteral("this_tab_only")).isEmpty()) {
            choice->tabNonce = freshNonce();
        }
    }
    qInfo("SynQt: the development picker signed somebody in as '%s'", qUtf8Printable(scope));
    return QHttpServerResponse{QByteArrayLiteral("text/plain"), QByteArrayLiteral("ok")};
}

}  // namespace SynQt
