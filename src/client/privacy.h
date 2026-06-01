// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_PRIVACY_H
#define SYNQT_PRIVACY_H

#include "synclientconfig.h"

#include <QJSValue>
#include <QObject>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QJSEngine;
QT_END_NAMESPACE

namespace SynQt {

/// What the app has to tell a visitor about their data, and what this visitor has said back.
/// It reads the project's `privacy:` block, and it is the accessor behind the `LegalFooter`,
/// `CookieConsent` and `DataErasureRequest` types, so a project that fills that block in gets
/// all three without writing any of this.
///
/// The framework sets one cookie, the session credential, and it is strictly necessary within
/// the meaning of the ePrivacy Directive Article 5(3) exemption. Without it the browser holds
/// no session and the app does not work. A project that adds nothing else needs no consent
/// banner, which is why `categories` is empty until the project declares a category and
/// `CookieConsent` renders nothing while it is.
class Privacy : public QObject
{
    Q_OBJECT

    /// Where the privacy policy and the legal notice live. An application route ("/privacy")
    /// or an absolute URL. Empty means the project declared none, and `LegalFooter` leaves
    /// that link out rather than pointing at a page that does not exist.
    Q_PROPERTY(QString policyUrl READ policyUrl CONSTANT)
    Q_PROPERTY(QString legalNoticeUrl READ legalNoticeUrl CONSTANT)

    /// The controller contact a visitor writes to, from `privacy.contact`. Empty when the
    /// project declared none.
    Q_PROPERTY(QString contact READ contact CONSTANT)

    /// How long the project keeps personal data, in days, from `privacy.retention_days`.
    /// Surfaced so a page can state the period without repeating a number that lives in the
    /// configuration.
    Q_PROPERTY(int retentionDays READ retentionDays CONSTANT)

    /// The non-essential cookie categories the project declared, empty in a project that
    /// declares none.
    Q_PROPERTY(QStringList categories READ categories CONSTANT)

    /// Whether anything needs asking. False while `categories` is empty: the session cookie
    /// is exempt, so there is nothing to ask about and no banner to show.
    Q_PROPERTY(bool consentRequired READ isConsentRequired CONSTANT)

    /// Whether this visitor has answered, and what they allowed. `granted` is a subset of
    /// `categories`. A visitor who accepted only what is necessary leaves it empty.
    Q_PROPERTY(bool consentAnswered READ isConsentAnswered NOTIFY consentChanged)
    Q_PROPERTY(QStringList granted READ granted NOTIFY consentChanged)

    /// The per-category check, as a function-valued property rather than a Q_INVOKABLE, so
    /// that `visible: Privacy.hasConsent("analytics")` re-evaluates when the visitor answers (see
    /// Session::hasScope).
    Q_PROPERTY(QJSValue hasConsent READ consentCheck NOTIFY consentChanged)

    /// Whether the project offers a signed-in visitor an erasure request
    /// (`privacy.erasure`). `DataErasureRequest` renders nothing when it does not, so the
    /// button exists only where somebody has undertaken to act on it.
    Q_PROPERTY(bool erasureOffered READ isErasureOffered CONSTANT)

public:
    /// \a engine is the app's own QML engine, and the only thing it builds is the
    /// `hasConsent` function above. A Privacy built without one answers every C++ caller;
    /// only the QML-side check needs an engine to exist in.
    explicit Privacy(SynClientConfig config, QJSEngine *engine = nullptr,
                     QObject *parent = nullptr);

    QString policyUrl() const;
    QString legalNoticeUrl() const;
    QString contact() const;
    int retentionDays() const;
    QStringList categories() const;
    bool isConsentRequired() const;
    bool isConsentAnswered() const;
    QStringList granted() const;
    QJSValue consentCheck() const;
    bool hasConsent(const QString &category) const;
    bool isErasureOffered() const;

public Q_SLOTS:
    /// Record what this visitor allows. Only the categories the project declared are kept,
    /// so a page cannot grant itself one that was never offered, and all three of these are
    /// an answer. A visitor who has answered is not asked again.
    void accept(const QStringList &categories);
    void acceptAll();
    void acceptNecessaryOnly();

    /// Forget the answer, so the banner asks again. Withdrawing has to be as easy as giving
    /// consent was (Article 7(3)), which is why this is on the accessor and not behind a
    /// page the app has to remember to build.
    void withdrawConsent();

Q_SIGNALS:
    void consentChanged();

private:
    void load();
    void store();

    SynClientConfig m_config;
    QJSEngine *m_engine{nullptr};
    QStringList m_granted;
    bool m_answered{false};
    /// Built on first read, which is why it is mutable. The read happens from a binding, on
    /// a const accessor, and there is no engine root to build it against any earlier.
    mutable QJSValue m_checkFunction;
};

/// Register `LegalFooter`, `CookieConsent` and `DataErasureRequest` into the `SynQt` module.
/// They are QML files in this library's resources rather than C++ types, because each one is
/// a handful of Controls and an app that wants a different look should be able to read what
/// it is replacing.
void registerPrivacyTypes();

} // namespace SynQt

#endif // SYNQT_PRIVACY_H
