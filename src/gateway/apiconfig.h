// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_APICONFIG_H
#define SYNQT_APICONFIG_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace SynQt {

/// The public HTTP surface one entity serves (`network.inbound`), and the whole of what
/// decides whether it serves one at all. An entity with no `network.inbound` builds no
/// ApiServer, opens no port, and is reachable only by the mesh consumers its connect
/// points list. Defaults here are the safe ones. The generated main writes only what the
/// topology declared.
struct ApiConfig
{
    /// Where it listens. There is no default port, because a public surface has to
    /// name the port it occupies, and `synqt check` refuses `inbound:` without one.
    QString host{QStringLiteral("0.0.0.0")};
    quint16 port{0};

    /// Public TLS. Empty means plaintext, which `synqt check` allows only when the
    /// deployment says a proxy terminates TLS in front of it.
    QString certFile;
    QString keyFile;

    /// Machine callers authenticate with a shared key in a header. The keys come from the
    /// entity's own environment (`api_keys: env:...`), never from a literal in the
    /// topology. An empty list is refused at `synqt check` unless the surface is written
    /// `public: true`, so an API is not left open by leaving a line out.
    QList<QByteArray> apiKeys;
    QByteArray keyHeader{QByteArrayLiteral("X-API-Key")};
    bool anonymous{false};  ///< `public: true`: no key required, said out loud

    /// Browser callers. A request carrying an `Origin` this list does not name is refused
    /// before a handler runs, and the preflight is answered for the ones it does name.
    /// Empty (the default) means no browser may call in, which is what a machine-facing
    /// API wants: a key in a page is not a secret.
    QStringList allowedOrigins;

    /// Resource limits, enforced before a handler sees anything.
    qint64 maxBodyBytes{1048576};
    int ratePerMinutePerIp{600};

    /// Peers whose `X-Forwarded-For` this surface believes, as addresses or CIDR ranges.
    /// Empty (the default) means the peer that connected is the caller, which is true of a
    /// surface reached directly and false of every request at once as soon as a proxy sits
    /// in front. There the rate limit above would count one address for everybody. Nothing
    /// is trusted implicitly, because a header any client can write would otherwise be a
    /// budget any client can pick. The browser side of an edge configures this separately
    /// (`public.trusted_proxies`). See SynQt::ClientAddress.
    QStringList trustedProxies;

    /// How long a handler may take to answer before the request is failed with 504.
    /// A handler answers on a later turn whenever it reaches a connect point or calls out
    /// (`Api.get("/x", r => Http.api("y").get(...).then(v => r.reply(v)))`), so the
    /// connection has to be held open for it. A handler that never answers must not hold
    /// it open forever. Zero is not a way to wait with no deadline. It falls back to the
    /// default and says so once (see ApiServer::handle).
    int replyTimeoutMs{15000};
};

} // namespace SynQt

#endif // SYNQT_APICONFIG_H
