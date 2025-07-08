// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CLIENTUPDATE_H
#define SYNQT_CLIENTUPDATE_H

#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace SynQt {

/// The QML `App` accessor: the running client itself, as opposed to `Server` (the connect
/// points), `Session` (who the user is), and `Router` (where they are).
///
/// The client is conveyed to every visitor, so a deploy can leave someone running an old
/// build for as long as their tab stays open. The shell cache notices and tells the page;
/// this decides what happens next. An app that handles `updateReady` owns the timing
/// (finish the round, save the draft, then call `applyUpdate()`). An app that handles
/// nothing gets an immediate reload, because an update nobody applies is worse than an
/// interruption.
///
/// \sa \ref qmlapp "the App accessor page"
class ClientUpdate : public QObject
{
    Q_OBJECT

public:
    explicit ClientUpdate(QObject *parent = nullptr);
    ~ClientUpdate() override;

    /// Reload onto the build the shell cache has already fetched. Instant: the worker
    /// cached it before raising updateReady.
    Q_INVOKABLE void applyUpdate();

    /// Called by the browser bridge when the shell cache reports a newer build.
    void notifyUpdateReady();

signals:
    void updateReady();

protected:
    /// Virtual because the library itself calls it (the only reason to make one virtual),
    /// and because it is the seam that lets the default be tested without a browser.
    virtual void reloadPage();
};

/// The `App` QML surface: the attached object behind `App.onUpdateReady` and
/// `App.applyUpdate()`.
///
/// It is the whole surface rather than half of it. Registering `App` as a QML type is what
/// makes the attached-handler syntax resolve, and a registered type shadows a context
/// property of the same name inside JS expressions, so a context-property `App` next to it
/// would leave `App.applyUpdate()` throwing "not a function". One name, one object: this
/// one, forwarding to the live ClientUpdate.
class ClientUpdateAttached : public QObject
{
    Q_OBJECT

public:
    explicit ClientUpdateAttached(QObject *parent = nullptr);

    Q_INVOKABLE void applyUpdate();

signals:
    void updateReady();
};

/// The attaching type, registered under the QML name "App" so `App.onUpdateReady`
/// resolves. It does nothing but provide the attached object, mirroring the generated
/// `<Contract>` attaching types.
class ClientUpdateAttachedType : public QObject
{
    Q_OBJECT
    QML_ATTACHED(ClientUpdateAttached)

public:
    explicit ClientUpdateAttachedType(QObject *parent = nullptr) : QObject{parent} {}

    static ClientUpdateAttached *qmlAttachedProperties(QObject *object)
    { return new ClientUpdateAttached{object}; }
};

/// Register the `App` attached type. The generated client main.cpp calls this before
/// loading QML; the context property named `App` is bound separately and is the same
/// object the attached one relays.
void registerClientUpdate();

} // namespace SynQt

#endif // SYNQT_CLIENTUPDATE_H
