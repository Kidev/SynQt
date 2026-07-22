// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "resumepath.h"

#include "routepattern.h"

#include <QVariantMap>

#ifdef Q_OS_WASM
#include <emscripten.h>

#include <cstdlib>
#endif

namespace SynQt {

#ifdef Q_OS_WASM

// Emscripten prunes any JS runtime helper nothing declares a need for, and
// it does not scan EM_JS bodies for the ones they call. Without this, the
// helpers below are in scope only as a side effect of what else the link
// happens to pull in, and losing that would throw a ReferenceError out of
// the guard path at runtime. Declared here, a missing helper is a link
// error instead. $stringToNewUTF8 carries its own dependency on malloc, so
// take() never names _malloc itself.
EM_JS_DEPS(synqt_resumepath, "$UTF8ToString,$stringToNewUTF8");

// sessionStorage is per tab and is never sent to the server, which is what
// makes it the right home for an intent that must outlive a navigation to
// the identity provider. Reached through EM_JS rather than emscripten::val
// so the try/catch stays on the JavaScript side: reading the property
// throws outright in a sandboxed frame, or where the browser is configured
// to block storage, and losing the resume target has to cost a worse
// landing page, never an abort.
EM_JS(void, synqt_resumepath_store, (const char *path), {
    try {
        window.sessionStorage.setItem("synqt.resume", UTF8ToString(path));
    } catch (error) {
    }
});

// Returns a freshly allocated UTF-8 string the caller frees, or nullptr if
// the allocation failed, empty when nothing was stored. Removing before
// returning is what makes the read destructive on this platform too.
EM_JS(char *, synqt_resumepath_take, (), {
    var value = "";
    try {
        value = window.sessionStorage.getItem("synqt.resume") || "";
        window.sessionStorage.removeItem("synqt.resume");
    } catch (error) {
    }
    return stringToNewUTF8(value);
});

#endif // Q_OS_WASM

namespace ResumePath {

namespace {

#ifndef Q_OS_WASM
/// The desktop store. The client runtime is single-threaded (the QML engine
/// and the QtRO node both live on the main thread), so this needs no lock.
QString s_stored;
#endif

/// True for a character that makes the path the matcher sees differ from
/// the path the browser ends up with.
bool hasRewritingCharacter(const QString &candidate)
{
    for (const QChar &character : candidate) {
        const char16_t code{character.unicode()};
        // Browsers strip TAB, LF and CR out of a URL before parsing it, so
        // "/\t/evil.example" arrives as "//evil.example": another origin.
        // The other control characters have no business in a route either.
        if (code < 0x20 || code == 0x7f) {
            return true;
        }
        // Several browsers fold a backslash to "/", which turns
        // "/\evil.example" into the same protocol-relative payload.
        if (code == u'\\') {
            return true;
        }
        // A colon introduces a scheme, so no candidate carrying one can be
        // the relative path this accepts.
        if (code == u':') {
            return true;
        }
        // The route table models no fragment, so a "#" here can only
        // smuggle something past the segment comparison below.
        if (code == u'#') {
            return true;
        }
    }
    return false;
}

/// True for a path segment the browser would rewrite or re-split.
bool hasUnsafeSegment(const QString &path)
{
    const QStringList segments{path.split(QLatin1Char('/'), Qt::KeepEmptyParts)};
    for (const QString &segment : segments) {
        // Dot segments are collapsed in the address bar, so the router and
        // the URL would disagree about which page is showing.
        if (segment == QLatin1String(".") || segment == QLatin1String("..")) {
            return true;
        }
        // "%2f" and "%5c" decode to a separator after the match is decided,
        // so what was matched is not what is navigated to. "%2e" is the
        // encoded dot: the URL standard counts ".%2e", "%2e." and "%2e%2e"
        // as double-dot segments and "%2e" as a single-dot one, so the
        // literal comparison above catches none of them. Refusing "%2e"
        // anywhere in a segment covers every spelling in one rule, at the
        // cost of a parameter that wanted an encoded dot.
        if (segment.contains(QLatin1String("%2f"), Qt::CaseInsensitive)
            || segment.contains(QLatin1String("%5c"), Qt::CaseInsensitive)
            || segment.contains(QLatin1String("%2e"), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool isAcceptable(const QString &candidate, const QStringList &declaredPaths)
{
    if (candidate.isEmpty() || candidate.size() > MaximumLength) {
        return false;
    }
    if (!candidate.startsWith(QLatin1Char('/'))) {
        return false;
    }
    // "//host" is protocol-relative: a browser reads it as another origin,
    // so accepting it is exactly the open redirect this guards against.
    if (candidate.startsWith(QLatin1String("//"))) {
        return false;
    }
    if (hasRewritingCharacter(candidate)) {
        return false;
    }
    QVariantMap query;
    const QString path{RoutePattern::splitQuery(candidate, &query)};
    if (hasUnsafeSegment(path)) {
        return false;
    }
    // The shape checks above deliberately duplicate work RoutePattern does:
    // matches() already refuses a relative path and every doubled slash of
    // its own. The two layers overlap on purpose. This one states what a
    // resume target may be, in one place, so a later change to the matcher
    // cannot quietly turn the resume into an open redirect.
    QVariantMap parameters;
    for (const QString &declared : declaredPaths) {
        const RoutePattern pattern{declared};
        if (pattern.matches(path, &parameters)) {
            return true;
        }
    }
    return false;
}

void store(const QString &path)
{
#ifdef Q_OS_WASM
    synqt_resumepath_store(path.toUtf8().constData());
#else
    s_stored = path;
#endif
}

QString take()
{
#ifdef Q_OS_WASM
    char *buffer{synqt_resumepath_take()};
    const QString taken{QString::fromUtf8(buffer)};
    std::free(buffer);
    return taken;
#else
    const QString taken{s_stored};
    s_stored.clear();
    return taken;
#endif
}

} // namespace ResumePath

} // namespace SynQt
