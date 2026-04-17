// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "clientupdate.h"

#include <QMetaMethod>
#include <QtQml/qqml.h>

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#endif

namespace SynQt {

// The one live accessor, constructed once for the page's lifetime; the browser bridge and
// every `App.onUpdateReady` attached object reach it without a pointer.
static ClientUpdate *s_instance{nullptr};

#ifdef __EMSCRIPTEN__

extern "C" EMSCRIPTEN_KEEPALIVE void synqt_client_update_ready()
{
    if (s_instance) {
        s_instance->notifyUpdateReady();
    }
}

// Install the hook the generated boot script looks for. Without the hook the boot script
// reloads on its own; the hook lets the app decide.
EM_JS(void, synqt_install_update_hook, (), {
    window.__synqtUpdateReady = function () { _synqt_client_update_ready(); };
});

EM_JS(void, synqt_reload_page, (), {
    window.location.reload();
});

#endif

ClientUpdate::ClientUpdate(QObject *parent)
    : QObject{parent}
{
    s_instance = this;
#ifdef __EMSCRIPTEN__
    synqt_install_update_hook();
#endif
}

ClientUpdate::~ClientUpdate()
{
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

void ClientUpdate::notifyUpdateReady()
{
    // If the app handles this, it decides when to apply. If nothing listens, apply now.
    static const QMetaMethod signal{QMetaMethod::fromSignal(&ClientUpdate::updateReady)};
    if (isSignalConnected(signal)) {
        emit updateReady();
        return;
    }
    reloadPage();
}

void ClientUpdate::applyUpdate()
{
    reloadPage();
}

void ClientUpdate::reloadPage()
{
#ifdef __EMSCRIPTEN__
    synqt_reload_page();
#endif
    // A native desktop client has no shell cache and no page to reload; it updates through
    // its installer.
}

ClientUpdateAttached::ClientUpdateAttached(QObject *parent)
    : QObject{parent}
{
    // Relay the live accessor's signal. One attached object is created per attachee, so
    // every handler fires.
    if (s_instance) {
        connect(s_instance, &ClientUpdate::updateReady,
                this, &ClientUpdateAttached::updateReady);
    }
}

void ClientUpdateAttached::applyUpdate()
{
    if (s_instance) {
        s_instance->applyUpdate();
    }
}

void registerClientUpdate()
{
    qmlRegisterType<ClientUpdateAttachedType>("SynQt", 1, 0, "App");
}

} // namespace SynQt
