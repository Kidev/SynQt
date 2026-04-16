// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SECRETS_H
#define SYNQT_SECRETS_H

#include <QByteArray>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QString>

namespace SynQt {

/// How many bytes of entropy every opaque value in this framework is made of. 256 bits: a
/// session credential, an OAuth state, a claim code and a device secret are all things
/// somebody would guess if guessing were worth trying, and none of them is ever so long
/// that a shorter one would buy anything.
inline constexpr int SecretBytes{32};

/// `SecretBytes` bytes from the system generator, hex-encoded.
///
/// The system generator and not the default one. `QRandomGenerator::global()` may be seeded
/// from a source that is only good enough for a hash table, and every caller here is minting
/// something that stands in for a person. A predictable state is a login somebody else can
/// finish, and a predictable device secret is a session somebody else can open.
///
/// This lives in one place because it was written in five, character for character. A single
/// copy is what makes "the framework mints secrets this way" a fact about the code rather
/// than about five places all still agreeing.
inline QByteArray randomSecret()
{
    QByteArray raw(SecretBytes, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(raw.data()),
                                          raw.size() / static_cast<qsizetype>(sizeof(quint32)));
    return raw.toHex();
}

/// The same value as a QString, for the callers that carry it through URLs and JSON.
inline QString randomToken()
{
    return QString::fromLatin1(randomSecret());
}

/// The PKCE S256 challenge for a verifier: its SHA-256 digest, base64url, unpadded.
///
/// The public half of a secret this file mints, so it lives beside the minting. It is also
/// computed on two machines -- the native client derives it from the verifier it is about to
/// keep, and the edge derives it again from the verifier presented at the claim, and compares
/// and those two computations must agree byte for byte or no desktop sign-in ever
/// completes. Written twice they agreed; written once they cannot do otherwise.
inline QByteArray challengeFor(const QByteArray &verifier)
{
    return QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

} // namespace SynQt

#endif // SYNQT_SECRETS_H
