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
QByteArray pageFor(const QStringList &scopeOrder)
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

QString IdentityPicker::route()
{
    return QStringLiteral("/synqt/dev/identity");
}

QHttpServerResponse IdentityPicker::page() const
{
    return QHttpServerResponse{QByteArrayLiteral("text/html; charset=utf-8"),
                               pageFor(m_scopeOrder)};
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

QHttpServerResponse IdentityPicker::choose(const QHttpServerRequest &request,
                                           Choice *choice)
{
    const QUrlQuery form{QString::fromUtf8(request.body())};
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
