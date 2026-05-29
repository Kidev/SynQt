// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_POLLINGDISPATCHER_H
#define SYNQT_POLLINGDISPATCHER_H

namespace SynQt {

/// Ask for Qt's polling event dispatcher rather than GLib's, on a platform that offers
/// both. Call it from main() **before** the QCoreApplication, which is when the dispatcher
/// is chosen. After that it does nothing.
///
/// Why an entity wants this. On Linux Qt uses QEventDispatcherGlib whenever GLib is there,
/// and GLib keeps every watched descriptor in one source's poll list. Enabling or disabling
/// a socket's write notifier adds to or removes from that list, and removal walks it. A
/// fan-out writes to every connection in one pass, so N sockets each toggle a notifier and
/// each toggle walks a list of length N: the cost of publishing one value to N subscribers
/// grows with the square of N, in the event loop rather than in anything SynQt or QtRO
/// wrote. The polling dispatcher keeps its notifiers in a hash and pays none of it.
///
/// What it costs. GLib's dispatcher is what lets a Qt program share a GLib main loop, which
/// in practice means GTK: the native file dialogs and colour dialogs a desktop application
/// gets from the GTK platform theme need it. That is a client's concern and not a service's,
/// so the generated main of a service, a web edge and a monitor calls this and the generated
/// main of a desktop client does not.
///
/// The environment wins if it has spoken. Qt reads QT_NO_GLIB as "any non-empty value means
/// no GLib", so putting GLib back is `QT_NO_GLIB=` with nothing after it, not `QT_NO_GLIB=0`.
void preferPollingEventDispatcher();

} // namespace SynQt

#endif // SYNQT_POLLINGDISPATCHER_H
