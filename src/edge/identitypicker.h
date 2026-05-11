// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Development-only, and this is the guard that says so at compile time rather than at link
// time. It sits above every #include so that a translation unit that reaches here in a
// release build fails naming the mistake rather than on whichever Qt header it could
// not find afterwards. See docs/security.md and tests/dev-exclusion.
#ifndef SYNQT_DEV_TOOLS
#error "identitypicker.h is development-only. It is compiled into SynQtEdge only when CMake is \
configured with -DSYNQT_DEV_TOOLS=ON, which `synqt dev` does and `synqt build` never does. \
If you are reading this from a release build, something is including a development \
header: fix the include rather than turning the option on."
#endif

#ifndef SYNQT_IDENTITYPICKER_H
#define SYNQT_IDENTITYPICKER_H

#include "webedgeconfig.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

QT_BEGIN_NAMESPACE
class QHttpServerRequest;
class QHttpServerResponse;
class QUrlQuery;
QT_END_NAMESPACE

namespace SynQt {

class SessionManager;

/// The development sign-in. One page listing the scopes the project declared, and a POST
/// that mints a session at whichever one was picked.
///
/// Its routes are registered only under `--identity-picker`, which only `synqt dev` passes,
/// so a guessed route name reaches nothing. `src/edge/CMakeLists.txt` compiles this file only
/// when `SYNQT_DEV_TOOLS` is on, so a release SynQtEdge does not contain the class
/// (tests/dev-exclusion reads both symbol tables).
///
/// It skips the OAuth flow entirely: no PKCE, no code exchange, no ID token, no JWKS.
/// `identity.dev_stub` exercises the flow.
class IdentityPicker : public QObject
{
    Q_OBJECT

public:
    IdentityPicker(SessionManager *sessions, QStringList scopeOrder,
                   QObject *parent = nullptr);

    /// Where the two routes live. One path, two methods: GET draws the page, POST takes
    /// the choice.
    static QString route();

    /// The named people from `.dev-identities`, already checked against this project's
    /// scopes by `synqt dev`, and one sentence per entry that did not survive that check.
    void setNamedIdentities(const QList<WebEdgeConfig::DevIdentity> &identities,
                            const QStringList &problems);

    /// What the project's own mapping hook makes of an identity, or empty with `error` set
    /// when it refuses one. Unset when this edge has no identity provider to ask, which is
    /// every project that has not configured a login. Then a named entry is worth exactly
    /// the scope its file gives it, and the page says so rather than implying a hook agreed.
    using ScopeMapper = std::function<QString(const QVariantMap &identity, QString *error)>;
    void setScopeMapper(ScopeMapper mapper);

    QHttpServerResponse page() const;

    /// What one POST asked for. A session, and whether it is this tab's alone.
    struct Choice
    {
        QByteArray sessionId;  ///< empty when the choice was refused
        /// Set when the visitor asked for a session scoped to this tab. The edge puts the
        /// session under `synqt_session_<nonce>` and sends the tab to `/?s=<nonce>`, so two
        /// tabs in one browser hold two sessions. Empty for the ordinary shared session.
        QByteArray tabNonce;
    };

    /// Take the choice. On success the returned Choice names the session that was minted;
    /// on refusal its `sessionId` is empty and the answer says why. The edge sets the
    /// cookie, because this class never forms one: there is one place that decides what a
    /// session cookie looks like on this edge, and the picker cannot drift from it.
    QHttpServerResponse choose(const QHttpServerRequest &request, Choice *choice);

private:
    /// The named half of `choose`, split out because the two halves share nothing but the
    /// per-tab checkbox. One resolves an index into the scope vocabulary, the other an
    /// index into a file, and through a mapping hook.
    QHttpServerResponse chooseNamed(const QString &picked, const QUrlQuery &form,
                                    Choice *choice);

    /// The identity a picked scope stands for. It cannot collide with anything
    /// a real provider issues. `sub` is `synqt-dev:<scope>:<epoch-ms>`, and a colon is not
    /// legal in a GitHub numeric id nor a prefix any OIDC issuer hands out.
    QVariantMap identityFor(const QString &scope) const;

    /// The identity a named entry stands for. The address is the one thing about it that is
    /// real, so it is what `sub` is built from. The same entry names the same person across
    /// restarts, which is the whole point of naming somebody instead of picking a scope.
    QVariantMap identityForNamed(const QString &email) const;

    /// What a named entry resolves to. The file's scope, unless a mapping hook exists and
    /// has an opinion, in which case the hook's answer wins and the disagreement is shown.
    struct Resolution
    {
        QString scope;    ///< empty when the hook refused this identity
        QString remark;   ///< what to show beside the entry. Empty when there is nothing to say
    };
    Resolution resolve(const WebEdgeConfig::DevIdentity &identity) const;

    SessionManager *m_sessions{nullptr};
    QStringList m_scopeOrder;
    QList<WebEdgeConfig::DevIdentity> m_named;
    QStringList m_problems;
    ScopeMapper m_mapper;
};

}  // namespace SynQt

#endif  // SYNQT_IDENTITYPICKER_H
