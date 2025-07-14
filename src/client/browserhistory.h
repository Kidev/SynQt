// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_BROWSERHISTORY_H
#define SYNQT_BROWSERHISTORY_H

#include <QObject>
#include <QString>
#include <QStringList>

namespace SynQt {

/// The browser's session history, and the one place in the client that knows
/// the browser has one.
///
/// Qt has no history API, so the WebAssembly build reaches the History API
/// through Emscripten while a desktop build keeps an equivalent stack in
/// memory. Both live here so Router needs no platform branch: back() works
/// in a native app for the same reason it works in a tab.
///
/// Paths crossing this class are application paths (always rooted at "/").
/// The base prefix an app is served under is applied on the way out and
/// stripped on the way in.
class BrowserHistory : public QObject
{
    Q_OBJECT

public:
    explicit BrowserHistory(QString basePath, QObject *parent = nullptr);
    ~BrowserHistory() override;

    /// The application path currently shown, taken from the address bar on
    /// a browser build so a deep link or a refresh starts on the right page.
    QString currentPath() const;

    void push(const QString &path);
    void replace(const QString &path);
    void back();
    void forward();

    QString toBrowserPath(const QString &applicationPath) const;
    QString toApplicationPath(const QString &browserPath) const;

public slots:
    /// Entry point for the browser's popstate event. Public because the
    /// Emscripten trampoline invokes it by name across the C boundary.
    void handlePopped(const QString &path);

signals:
    /// The visitor moved through history (the back or forward button, or a
    /// gesture).
    void popped(const QString &path);

private:
    void notifyPopped(const QString &path);

    QString m_base;
    // The desktop stack. Unused on a browser build, where the browser owns
    // the history.
    QStringList m_stack;
    int m_index{0};
};

} // namespace SynQt

#endif // SYNQT_BROWSERHISTORY_H
