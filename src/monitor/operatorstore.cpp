// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "operatorstore.h"

#include <QCryptographicHash>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QRegularExpression>

namespace SynQt {

namespace {

constexpr int kSaltBytes{16};
constexpr int kHashBytes{32};

/// Compare without letting the time it takes say how much of the value was right.
///
/// A monitor is reachable by whoever can reach its port, and a comparison that returns
/// early on the first wrong byte tells them, one byte at a time, what the right answer is.
bool equalInConstantTime(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size()) {
        return false;
    }
    quint8 difference{0};
    for (qsizetype index{0}; index < left.size(); ++index) {
        difference |= static_cast<quint8>(left.at(index) ^ right.at(index));
    }
    return difference == 0;
}

QByteArray derive(const QString &password, const QByteArray &salt, int iterations)
{
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256,
                                              password.toUtf8(), salt, iterations,
                                              kHashBytes);
}

} // namespace

const char *OperatorStore::credentialVariable()
{
    return "SYNQT_MONITOR_OPERATORS";
}

OperatorStore::OperatorStore() = default;

QString OperatorStore::mint(const QString &name, const QString &password, int iterations)
{
    QByteArray salt(kSaltBytes, Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(salt.data()),
                                         reinterpret_cast<quint32 *>(salt.data()
                                                                     + salt.size()));
    const int rounds{qMax(MinimumIterations, iterations)};
    return QStringLiteral("%1:%2:%3:%4")
        .arg(name)
        .arg(rounds)
        .arg(QString::fromLatin1(salt.toHex()),
             QString::fromLatin1(derive(password, salt, rounds).toHex()));
}

bool OperatorStore::add(const QString &entry, QString *error)
{
    const QStringList parts{entry.split(QLatin1Char(':'))};
    if (parts.size() != 4) {
        if (error) {
            *error = QStringLiteral("expected name:iterations:salt:hash, got '%1'")
                         .arg(entry.left(24));
        }
        return false;
    }
    Credential credential;
    credential.name = parts.at(0).trimmed();
    bool ok{false};
    credential.iterations = parts.at(1).toInt(&ok);
    credential.salt = QByteArray::fromHex(parts.at(2).toLatin1());
    credential.hash = QByteArray::fromHex(parts.at(3).toLatin1());
    if (credential.name.isEmpty() || !ok || credential.salt.isEmpty()
            || credential.hash.isEmpty()) {
        if (error) {
            *error = QStringLiteral("operator '%1' has a malformed credential")
                         .arg(credential.name);
        }
        return false;
    }
    if (credential.iterations < MinimumIterations) {
        // Refused rather than accepted and warned about. A credential whose weakness is
        // only mentioned in a log line stays as weak as the day it was written.
        if (error) {
            *error = QStringLiteral("operator '%1' was derived with %2 iterations; the "
                                    "minimum is %3")
                         .arg(credential.name)
                         .arg(credential.iterations)
                         .arg(MinimumIterations);
        }
        return false;
    }
    m_credentials.append(credential);
    return true;
}

bool OperatorStore::loadFromEnvironment(QString *error)
{
    const QByteArray raw{qgetenv(credentialVariable())};
    if (raw.isEmpty()) {
        return true;   // no operators, and verify() refuses everybody
    }
    const QStringList entries{QString::fromUtf8(raw).split(
        QRegularExpression{QStringLiteral("[\\s,]+")}, Qt::SkipEmptyParts)};
    QStringList problems;
    for (const QString &entry : entries) {
        QString reason;
        if (!add(entry, &reason)) {
            problems.append(reason);
        }
    }
    if (!problems.isEmpty()) {
        if (error) {
            *error = problems.join(QStringLiteral("; "));
        }
        return false;
    }
    return true;
}

bool OperatorStore::verify(const QString &name, const QString &password) const
{
    // No operators means no operators. An empty store letting everybody in is how an
    // operations console ends up with no gate on it at all.
    if (m_credentials.isEmpty()) {
        return false;
    }

    // The whole list is read, and the first match is remembered rather than returned from.
    // Returning early on a name that is not there is what made the gate answer an unknown
    // operator in microseconds and a known one in a PBKDF2, which tells whoever is guessing
    // which names exist -- the one thing the single "no" the sign-in route answers with is
    // there to withhold.
    const Credential *found{nullptr};
    for (const Credential &credential : m_credentials) {
        if (found == nullptr && credential.name == name) {
            found = &credential;
        }
    }

    // One derivation either way, and of the same shape: an unknown name is worked against
    // the first credential's salt and round count, so the cost of a wrong guess does not
    // depend on which half of it was wrong.
    const Credential &against{found != nullptr ? *found : m_credentials.first()};
    const QByteArray derived{derive(password, against.salt, against.iterations)};
    const bool digestMatches{equalInConstantTime(derived, against.hash)};
    return (found != nullptr) && digestMatches;
}

QStringList OperatorStore::names() const
{
    QStringList names;
    names.reserve(m_credentials.size());
    for (const Credential &credential : m_credentials) {
        names.append(credential.name);
    }
    return names;
}

bool OperatorStore::isEmpty() const
{
    return m_credentials.isEmpty();
}

} // namespace SynQt
