// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Development-only, and this is the guard that says so at compile time rather than at link
// time. It sits above every #include so that a translation unit that reaches here in a
// release build fails naming the mistake rather than on whichever Qt header it could
// not find afterwards. See docs/security.md and tests/dev-exclusion.
#ifndef SYNQT_DEV_TOOLS
#error "identitypicker.h is development-only. It is compiled into SynQtEdge only when CMake is configured with -DSYNQT_DEV_TOOLS=ON, which `synqt dev` does and `synqt build` never does. If you are reading this from a release build, something is including a development header: fix the include rather than turning the option on."
#endif

#ifndef SYNQT_IDENTITYPICKER_H
#define SYNQT_IDENTITYPICKER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

QT_BEGIN_NAMESPACE
class QHttpServerRequest;
class QHttpServerResponse;
QT_END_NAMESPACE

namespace SynQt {

class SessionManager;

/// The development sign-in. One page listing the scopes the project declared, and a POST
/// that mints a session at whichever one was picked.
///
/// It exists behind `--identity-picker`, which `synqt dev` is the only launcher to pass,
/// and its routes are registered only when that flag is set. A route that is absent cannot
/// be reached by a request that guesses its name, which a route that refuses can. That is
/// the runtime layer. The compile-time layer is this file not being named by
/// `src/edge/CMakeLists.txt` unless `SYNQT_DEV_TOOLS` is on, so a release SynQtEdge does
/// not contain the class at all (tests/dev-exclusion reads both symbol tables).
///
/// It skips the OAuth flow entirely: no PKCE, no code exchange, no ID token, no JWKS. That
/// is the trade it makes for speed, and the reason `identity.dev_stub` is kept beside it
/// rather than replaced. The stub proves the flow. This one skips it.
class IdentityPicker : public QObject
{
    Q_OBJECT

public:
    IdentityPicker(SessionManager *sessions, QStringList scopeOrder,
                   QObject *parent = nullptr);

    /// Where the two routes live. One path, two methods: GET draws the page, POST takes
    /// the choice.
    static QString route();

    QHttpServerResponse page() const;

    /// Take the choice. On success `*sessionId` is the session that was minted and the
    /// answer is 200; on refusal it is left empty and the answer says why. The edge sets
    /// the cookie, because this class never forms one: there is one place that decides what
    /// a session cookie looks like on this edge, and the picker cannot drift from it.
    QHttpServerResponse choose(const QHttpServerRequest &request, QByteArray *sessionId);

private:
    /// The identity a picked scope stands for. Deliberately unable to collide with anything
    /// a real provider issues: `sub` is `synqt-dev:<scope>:<epoch-ms>`, and a colon is not
    /// legal in a GitHub numeric id nor a prefix any OIDC issuer hands out.
    QVariantMap identityFor(const QString &scope) const;

    SessionManager *m_sessions{nullptr};
    QStringList m_scopeOrder;
};

}  // namespace SynQt

#endif  // SYNQT_IDENTITYPICKER_H
