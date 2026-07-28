// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_BOUNDEDREPLY_H
#define SYNQT_BOUNDEDREPLY_H

#include <QtNetwork/qnetworkreply.h>
#include <QtNetwork/qnetworkrequest.h>

namespace SynQt {

/// Aborts \a reply, marked `synqtTooLarge`, as soon as its answer is known to be larger than
/// \a limit bytes: on the announced length when the headers arrive, and on what has arrived
/// at every read, which catches a chunked answer that announces nothing.
///
/// Not on downloadProgress: Qt throttles that signal, so an oversized answer could run past
/// the limit, or be refused by the deadline instead of by its size.
inline void refuseAnswersLargerThan(QNetworkReply *reply, qint64 limit)
{
    const auto refuse{[reply]() {
        reply->setProperty("synqtTooLarge", true);
        reply->abort();
    }};
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, limit, refuse]() {
        const QVariant announced{reply->header(QNetworkRequest::ContentLengthHeader)};
        if (announced.isValid() && announced.toLongLong() > limit) {
            refuse();
        }
    });
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply, limit, refuse]() {
        if (reply->bytesAvailable() > limit) {
            refuse();
        }
    });
}

} // namespace SynQt

#endif // SYNQT_BOUNDEDREPLY_H
