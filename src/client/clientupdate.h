// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CLIENTUPDATE_H
#define SYNQT_CLIENTUPDATE_H

#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace SynQt {

/// The QML \qmlApp accessor: the running client itself, as opposed to \qmlServer,
/// \qmlSession and \qmlRouter.
///
/// When the shell cache sees a new build, an app that handles `updateReady` picks the moment
/// and calls `applyUpdate()`; an app that handles nothing reloads immediately.
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

/// The \qmlApp QML surface: the attached object behind `App.onUpdateReady` and
/// `App.applyUpdate()`. The whole surface, since a registered type shadows a context property
/// of the same name in JS expressions.
class ClientUpdateAttached : public QObject
{
    Q_OBJECT

public:
    explicit ClientUpdateAttached(QObject *parent = nullptr);

    Q_INVOKABLE void applyUpdate();

signals:
    void updateReady();
};

/// The attaching type, registered under the QML name \qmlApp so `App.onUpdateReady`
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

/// Register the \qmlApp attached type. The generated client main.cpp calls this before
/// loading QML; the context property named `App` is bound separately and is the same
/// object the attached one relays.
void registerClientUpdate();

} // namespace SynQt

#endif // SYNQT_CLIENTUPDATE_H
