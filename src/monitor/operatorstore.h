// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_OPERATORSTORE_H
#define SYNQT_OPERATORSTORE_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace SynQt {

/// Who may read the console.
///
/// The monitor holds every entity's record, which makes it the one place in a system where
/// a single sign-in reveals what everything has been doing. So it has an identity system of
/// its own, deliberately separate from the application's: an operator is not a user of the
/// application, and a project's own login provider is often the thing an operator is
/// signing in to investigate.
///
/// Credentials live in the monitor entity's environment and never in `synqt.yaml`, which
/// is a file in a repository. The stored form is a PBKDF2-SHA256 hash with a per-operator
/// salt. Nothing here ever holds, logs or records a password.
///
/// Fail closed. A monitor with no operators configured refuses everybody. The opposite
/// default, letting everyone in until somebody is configured, is how an operations console
/// ends up on a network with no gate at all.
class OperatorStore
{
public:
    /// The environment variable the operator list is read from
    /// (`name:iterations:saltHex:hashHex`, entries separated by whitespace or commas).
    static const char *credentialVariable();

    /// The iteration count a stored credential must meet. Anything weaker is refused at
    /// load rather than accepted quietly, because a credential nobody checks the strength
    /// of is one that stays as weak as the day it was written.
    static constexpr int MinimumIterations{600000};

    OperatorStore();

    /// Read the operator list from the process environment. Returns false with a reason
    /// when an entry is malformed. Entries that parse are still loaded, so one bad line
    /// does not lock every operator out.
    bool loadFromEnvironment(QString *error = nullptr);

    /// Take one entry in the stored form, for tests and for `synqt monitor operator add`.
    bool add(const QString &entry, QString *error = nullptr);

    /// Is this the right password for this operator? False for an unknown name, a wrong
    /// password, and an empty store.
    bool verify(const QString &name, const QString &password) const;

    QStringList names() const;
    bool isEmpty() const;

    /// The stored form of a fresh credential: `name:iterations:saltHex:hashHex`. Used by
    /// the CLI to print a line an operator pastes into the monitor's environment. The
    /// password is not part of what it returns and is not kept anywhere.
    static QString mint(const QString &name, const QString &password,
                        int iterations = MinimumIterations);

private:
    struct Credential
    {
        QString name;
        int iterations{0};
        QByteArray salt;
        QByteArray hash;
    };

    QList<Credential> m_credentials;
};

} // namespace SynQt

#endif // SYNQT_OPERATORSTORE_H
