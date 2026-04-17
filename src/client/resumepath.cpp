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

// Emscripten prunes JS runtime helpers nothing declares, and it does not scan EM_JS bodies.
// Declaring them here turns a missing helper into a link error instead of a runtime
// ReferenceError on the guard path. $stringToNewUTF8 declares its own dependency on malloc,
// so take() does not name _malloc.
EM_JS_DEPS(synqt_resumepath, "$UTF8ToString,$stringToNewUTF8");

// sessionStorage is per tab and never sent to the server, so it can hold an intent across
// the navigation to the identity provider. Reached through EM_JS rather than
// emscripten::val so the try/catch stays in JavaScript: reading it throws in a sandboxed
// frame or when storage is blocked, and that must only cost the resume target.
EM_JS(void, synqt_resumepath_store, (const char *path), {
    try {
        window.sessionStorage.setItem("synqt.resume", UTF8ToString(path));
    } catch (error) {
    }
});

// Returns a newly allocated UTF-8 string the caller frees, nullptr if the allocation
// failed, empty when nothing was stored. The value is removed before returning, so the read
// is destructive.
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
/// The desktop store. The client runtime is single-threaded (the QML engine and the QtRO
/// node share the main thread), so no lock.
QString s_stored;
#endif

/// True for a character that would make the matched path differ from the path the browser
/// uses.
bool hasRewritingCharacter(const QString &candidate)
{
    for (const QChar &character : candidate) {
        const char16_t code{character.unicode()};
        // Browsers strip TAB, LF and CR from a URL, so "/\t/evil.example" becomes
        // "//evil.example", another origin. Other control characters do not belong in a
        // route either.
        if (code < 0x20 || code == 0x7f) {
            return true;
        }
        // Several browsers fold a backslash to "/", turning "/\evil.example" into the same
        // protocol-relative URL.
        if (code == u'\\') {
            return true;
        }
        // A colon introduces a scheme, so a candidate containing one cannot be a relative
        // path.
        if (code == u':') {
            return true;
        }
        // Routes have no fragment, so a "#" could only smuggle something past the segment
        // comparison.
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
        // The address bar collapses dot segments, so the router and the URL would disagree
        // about the page.
        if (segment == QLatin1String(".") || segment == QLatin1String("..")) {
            return true;
        }
        // "%2f" and "%5c" decode to a separator after matching, so the matched path is not
        // the navigated one. "%2e" is an encoded dot, and ".%2e", "%2e." and "%2e%2e" count
        // as double-dot segments, which the literal comparison misses. Refusing "%2e"
        // anywhere covers every spelling, at the cost of parameters containing an encoded
        // dot.
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
    // "//host" is protocol-relative: another origin, an open redirect.
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
    // These shape checks overlap RoutePattern, whose matches() already refuses relative
    // paths and doubled slashes. They are kept here so a later change to the matcher cannot
    // turn the resume into an open redirect.
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
