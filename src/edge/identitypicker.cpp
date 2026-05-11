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

/// The picker's own page. Plain HTML with no script and no styling framework, because it
/// is served by a development edge whose CSP is the project's own: a page that needed an
/// inline script would be a page that only works when the project has relaxed its policy,
/// which is the opposite of what a development tool should demand. One form per scope, so
/// the choice is an ordinary POST and needs nothing but the browser.
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
                // The per-tab box is read by the form it sits in, so whichever scope button
                // is pressed carries the checkbox beside it. One box per form rather than
                // one for the page, because a single box outside every form is a box no
                // form submits.
                "<label><input type=\"checkbox\" name=\"this_tab_only\" value=\"1\" "
                "id=\"this-tab-only-" + scope.toHtmlEscaped().toUtf8()
                + "\"> this tab only</label>\n"
                "<button type=\"submit\" data-scope=\"" + scope.toHtmlEscaped().toUtf8()
                + "\">" + scope.toHtmlEscaped().toUtf8() + "</button>\n"
                "</form>\n";
    }

    // The named people from `.dev-identities`, if there are any. Second, because picking a
    // scope is the mode that always works and this one exists only when a file says so.
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

    // And what was in the file and could not be used. On the page rather than only in the
    // terminal. A name that is missing is noticed here, by somebody looking for it.
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

/// A name for one tab's cookie. Random rather than counted, so two developers on one edge
/// do not collide, and letters and digits only because it lands in a cookie name (WebEdge
/// validates the same alphabet on the way back in, and refuses anything else).
///
/// Not a credential. It says which cookie to read, and the cookie still holds the session
/// id. Generated with QRandomGenerator::system() anyway, because a value that is trivially
/// predictable invites the next reader to start treating it as one.
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
    // The hook is consulted here, while the page is drawn, rather than only when a name is
    // pressed. Seeing what the project's own mapping makes of somebody is the reason to
    // name them, and a disagreement between the file and the hook is worth reading before
    // choosing, not after.
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
    // Stable across restarts, unlike the scope mode's timestamped `sub`: a project that
    // stores anything against a person must see the same person on the next run, which is
    // most of what naming one is for. Still unable to collide with a real provider's id,
    // for the same reason and by the same prefix.
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
        // No identity provider on this edge, so there is no hook to ask. Say that, rather
        // than showing the file's scope alone and letting it read as a hook that agreed.
        return {identity.scope, QStringLiteral("%1 (from the file; this project has no "
                                               "mapping hook to ask)").arg(identity.scope)};
    }

    QString error;
    const QString mapped{m_mapper(identityForNamed(identity.email), &error)};
    if (mapped.isEmpty()) {
        // The hook refused this person, which is what a real login would do with them. The
        // picker refuses too: a development sign-in that granted what the project's own
        // rule denies would be showing a state the application cannot reach.
        return {QString{}, QStringLiteral("refused by the mapping hook: %1").arg(error)};
    }
    if (mapped != identity.scope) {
        // The disagreement is what matters, so both are shown and the hook's answer
        // is the one the session gets.
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
    // Null, not absent and not invented. `identity.email` is nullable for a real provider
    // too, and a hook that keys authorization on it has to behave the same here as it does
    // when GitHub declines to give one (docs/authentication.md).
    identity.insert(QStringLiteral("email"), QVariant{});
    return identity;
}

QHttpServerResponse IdentityPicker::chooseNamed(const QString &picked,
                                                const QUrlQuery &form, Choice *choice)
{
    // An index into the list this page drew, bounds-checked exactly as a posted scope is:
    // the list came from a file, so a larger number posted by hand must not reach past it.
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
        // The project's own hook refused this person. Refusing here too is the only right
        // answer, because signing them in anyway would show a state a real login cannot produce.
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   resolution.remark.toUtf8(),
                                   QHttpServerResponder::StatusCode::Forbidden};
    }
    // Belt and braces on top of the hook's own bounds check, because a mapper that is not
    // the identity provider's could be set here one day and this is the only place that
    // would notice.
    if (!m_scopeOrder.contains(resolution.scope)) {
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("not one of this project's scopes"),
                                   QHttpServerResponder::StatusCode::BadRequest};
    }

    const QByteArray minted{m_sessions->createSession(resolution.scope,
                                                      identityForNamed(identity.email))};
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

    // An index into the declared vocabulary, exactly as a mapping hook's answer is, and
    // bounds-checked the same way. The picker skips the hook (picking a scope directly is
    // the whole point of this mode), so this is the only thing standing between the form
    // and the session. A scope the project never declared must not be reachable by editing
    // the page and posting a larger number.
    bool isNumber{false};
    const int index{picked.toInt(&isNumber)};
    if (!isNumber || index < 0 || index >= static_cast<int>(m_scopeOrder.size())) {
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("not one of this project's scopes"),
                                   QHttpServerResponder::StatusCode::BadRequest};
    }

    const QString scope{m_scopeOrder.at(index)};
    const QByteArray minted{m_sessions->createSession(scope, identityFor(scope))};
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
