// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_PROXYPOLICY_H
#define SYNQT_PROXYPOLICY_H

class QNetworkAccessManager;

namespace SynQt {

/// Give `network` the egress route a server takes. The one its own environment names, and
/// otherwise none.
///
/// Qt's default is the machine's proxy configuration, which is right for a desktop
/// application sitting in somebody's session and wrong for an entity running as a service.
/// Two reasons, and the second is why this exists at all:
///
///   * A service has no user whose browser settings speak for it. Every other server
///     runtime (curl, Python, Node, Go) reads `HTTPS_PROXY`, `HTTP_PROXY`, `ALL_PROXY` and
///     `NO_PROXY` from its own environment, which is also how a container is configured.
///     An entity that took the machine's settings instead would route differently
///     depending on who was logged in.
///   * On Windows, resolving the system configuration runs WinHTTP's proxy auto-detection
///     (WPAD), which Qt's own documentation warns "may take several seconds to execute
///     depending on the configuration of the user's system" -- and it runs on the calling
///     thread, at the first request to a non-local host. An entity whose QML calls out
///     while it is starting therefore holds up everything it has not yet started, its
///     inbound listener included. That is where this was found: a gateway that called one
///     URL from `Component.onCompleted` took forty seconds to begin serving.
///
/// So the policy is explicit and platform-independent: the environment names the proxy or
/// there is none. `NO_PROXY` is honoured as the list of hosts (or `.suffix` entries, or
/// `*`) that are reached directly anyway.
void applyEnvironmentProxy(QNetworkAccessManager *network);

} // namespace SynQt

#endif // SYNQT_PROXYPOLICY_H
