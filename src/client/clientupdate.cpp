// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "clientupdate.h"

#include <QMetaMethod>
#include <QtQml/qqml.h>

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#endif

namespace SynQt {

// The one live accessor. The client runtime constructs exactly one, for the lifetime of
// the page, and both the browser bridge and every `App.onUpdateReady` attached object
// need to reach it without being handed a pointer.
static ClientUpdate *s_instance{nullptr};

#ifdef __EMSCRIPTEN__

extern "C" EMSCRIPTEN_KEEPALIVE void synqt_client_update_ready()
{
    if (s_instance) {
        s_instance->notifyUpdateReady();
    }
}

// Install the hook the generated boot script looks for. The boot script reloads on its
// own when the hook is absent, so a client that never constructs this still updates; the
// hook only exists to hand the decision to the app.
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
    // The documented default as a mechanism rather than a note: if the app took the
    // trouble to handle this, it owns the timing. If nothing is listening, apply now,
    // because an update nobody applies is worse than an interruption.
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
    // A native desktop client has no shell cache and no page to reload, so this is
    // deliberately a no-op there: it updates through its own installer instead.
}

ClientUpdateAttached::ClientUpdateAttached(QObject *parent)
    : QObject{parent}
{
    // Relay the live accessor's signal. The attached object is created per attachee, so
    // several handlers across a document each get their own and all fire.
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
