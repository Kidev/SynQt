// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_TRACEEVENT_H
#define SYNQT_TRACEEVENT_H

#include <QString>
#include <QVariantMap>

namespace SynQt {

/// How much an event matters, on the OpenTelemetry ladder.
enum class Severity { Trace, Debug, Info, Warning, Error, Fatal };

/// What an event is about. The vocabulary is closed because an operator filters by
/// these, and a free-form string would make the filter list depend on what happened to
/// have been logged today.
enum class Category {
    Lifecycle,      ///< an entity or a link starting, stopping, reconnecting
    Transport,      ///< an upgrade, a mesh handshake, a socket closing
    Authorization,  ///< a refusal, a scope check, a session elevation
    Call,           ///< a slot crossing a link
    Data,           ///< a model publish, a property push, a provider query
    Application,    ///< whatever an entity's own QML says through Log
};

/// One thing that happened, in the shape of the OpenTelemetry log and span model, so that
/// exporting to a third-party collector is a serialization rather than a translation.
///
/// A record, never a formatted string: an operator filters on `entity` and `category` and
/// searches `attributes`. `message` is the human sentence, and it carries no fact that is not
/// also an attribute.
struct TraceEvent
{
    qint64 timestampMs{0};
    Severity severity{Severity::Info};
    Category category{Category::Lifecycle};
    QString entity;
    /// W3C trace context. Empty on an event that begins nothing and continues nothing.
    QString traceId;
    QString spanId;
    QString parentSpanId;
    /// How long the span this event ended took, or -1 when the event ended no span.
    qint64 durationUs{-1};
    bool ok{true};
    QString message;
    QVariantMap attributes;
    /// Whether this is a browser-reported fact rather than one this process saw. A
    /// browser is untrusted. Nothing may treat a client-reported entity name as an
    /// identity (docs/security.md, the two identity systems).
    bool untrusted{false};

    QVariantMap toVariant() const;
    static TraceEvent fromVariant(const QVariantMap &value);
};

/// The names an operator reads, and the names a third-party collector expects.
///
/// The enums cross the link as their numbers; these turn one back into a word for a console
/// listing and an exported record.
QString severityName(Severity severity);
QString categoryName(Category category);

/// The reverse, for a word that came from a configuration file. False when it is not one of
/// them, so a misspelled level is reported rather than read as the quietest thing it could
/// have meant.
bool severityFromName(const QString &name, Severity *severity);
bool categoryFromName(const QString &name, Category *category);

} // namespace SynQt

#endif // SYNQT_TRACEEVENT_H
