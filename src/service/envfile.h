// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_ENVFILE_H
#define SYNQT_ENVFILE_H

#include <QString>

namespace SynQt {

/// The outcome of loading an entity's env file. A missing file is not a failure: an
/// entity is meant to run under a real secret store (systemd, a container's environment,
/// a CI secret) with no file on disk at all, so `NotFound` is the ordinary deployment
/// case and only `Unreadable` is worth reporting.
enum class EnvFileResult { Loaded, NotFound, Unreadable };

/// Load an entity's env file (`env: { file: web/.env }`) into this process's
/// environment, so a configured `env:VAR` reference resolves.
///
/// `KEY=VALUE` per line; `#` starts a comment; blank lines are skipped; a value may be
/// wrapped in matching single or double quotes, which are stripped. A variable already
/// present in the environment is never overwritten: the real environment is the
/// authority (a deployment's own secret store must win over a file that happened to be
/// copied along with the binary), and the file only fills what it left unset.
///
/// Never called from a client target. Secrets belong to the service side of the system,
/// and the browser is on the other side of the connect-point boundary.
EnvFileResult loadEnvFile(const QString &path, int *loadedCount = nullptr);

} // namespace SynQt

#endif // SYNQT_ENVFILE_H
