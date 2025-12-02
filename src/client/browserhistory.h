// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_BROWSERHISTORY_H
#define SYNQT_BROWSERHISTORY_H

#include <QObject>
#include <QString>
#include <QStringList>

namespace SynQt {

/// Leave the app for `url`, as a real navigation and not a route change, as signing in and
/// out do. A desktop build hands the URL to the system browser (RFC 8252) and returns false.
bool leaveForUrl(const QString &url);

/// The browser's session history. The WebAssembly build uses the History API through
/// Emscripten; a desktop build keeps an equivalent stack in memory, so Router needs no
/// platform branch. Paths here are application paths; the base prefix is applied on the way
/// out and stripped on the way in.
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
