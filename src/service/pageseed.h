// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_PAGESEED_H
#define SYNQT_PAGESEED_H

#include <QObject>

namespace SynQt {

/// The base type a page seed hook (a route's `seed:` QML file) derives from. The hook adds
/// `function seedFor(route, parameters, caller)`; the edge invokes it after the page's scope
/// check to build the data the delivered page paints with on its first frame, before its
/// connect points have pushed anything. It runs only on the edge.
///
/// Because it runs after the check it may read privileged state, and it is handed the caller
/// precisely so it can scope what it returns. Whatever it returns is sent to the browser, so
/// treat its return value as public output of a privileged context.
class PageSeed : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
};

} // namespace SynQt

#endif // SYNQT_PAGESEED_H
