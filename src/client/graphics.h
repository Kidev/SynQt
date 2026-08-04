// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_GRAPHICS_H
#define SYNQT_GRAPHICS_H

#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QQmlComponent;
class QQmlEngine;
QT_END_NAMESPACE

namespace SynQt {

/// What the client knows about the scene graph it got, exposed to QML as \c Graphics.
///
/// Two properties, both read only. An app that wants to react without replacing the notice
/// binds to them; an app that wants neither gets the notice anyway.
class Graphics : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isSoftwareRendered READ isSoftwareRendered CONSTANT)
    Q_PROPERTY(bool hasUnsupportedContent READ hasUnsupportedContent
               NOTIFY unsupportedContentFound)

public:
    explicit Graphics(QObject *parent = nullptr);
    ~Graphics() override;

    bool isSoftwareRendered() const;
    bool hasUnsupportedContent() const;

    /// Watch for Qt declining to draw something, and chain to the handler already
    /// installed. Chaining is required: ClientLogging leaves Qt's own handler in place
    /// under `client_logging: qt`, so this cannot assume one of ours exists, and it must
    /// not swallow what the other one would have printed.
    void installWatcher();

    /// True for a message that says Qt refused to draw something for want of an
    /// accelerated scene graph. Public so it can be tested without one.
    static bool isRefusal(const QString &message);

    /// The notice component: \a overrideUrl when the app named one, else the built-in.
    /// The caller owns the result.
    static QQmlComponent *noticeComponent(QQmlEngine *engine, const QString &overrideUrl,
                                          QObject *parent);

    /// Where to put the notice when the watcher fires: over \a rootWindow, leaving what
    /// did render on screen. Without this the flag is still set and QML can bind to it,
    /// but nothing appears on its own.
    void attachTo(QObject *rootWindow, QQmlEngine *engine, const QString &noticeUrl);

signals:
    void unsupportedContentFound();

private:
    void reportUnsupportedContent();
    void showNotice();

    bool m_unsupportedContent{false};
    QObject *m_rootWindow{nullptr};
    QQmlEngine *m_engine{nullptr};
    QString m_noticeUrl;
    QObject *m_notice{nullptr};
};

} // namespace SynQt

#endif // SYNQT_GRAPHICS_H
