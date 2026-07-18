// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The sessions baseline (M7): the cost of the edge's session hot path as the number of live
// sessions grows. Every browser WebSocket upgrade looks a session up by its credential in the
// verifier, and every scoped slot re-checks the caller's scope, so these two operations sit on
// the request path and must stay flat as the edge fills with sessions. This harness stands up
// a real SynQt::SessionManager, fills it to N live sessions, and measures:
//
//   * lookup_hit: SessionManager::lookup() of a live credential (the per-upgrade cost);
//   * lookup_miss: lookup() of an unknown credential (the rejection path);
//   * hasScope_set: Caller::hasScope() with a set-based vocabulary (exact-match);
//   * hasScope_hier-- Caller::hasScope() with a hierarchical vocabulary (rank compare);
//   * createSession-- minting a session (random token + insert);
//   * snapshot: the full-table snapshot the edge replays to a late-joining consumer.
//
// The first five are sub-microsecond, so they are reported as throughput (ns/op from a large
// batch, the honest way to time ns-scale operations), swept over N to show O(1) behaviour;
// snapshot grows with N and is reported per call. Results are written as a committed baseline.

#include "caller.h"
#include "sessionmanager.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QStringList>
#include <QSysInfo>
#include <QTextStream>
#include <QVariantList>

using SynQt::Caller;
using SynQt::SessionManager;

namespace {

// The scope vocabulary, low -> high. Sessions are given scopes cycled from it; hierarchical
// checks compare rank, set-based checks compare for equality.
const QStringList kScopeOrder{QStringLiteral("anonymous"), QStringLiteral("viewer"),
                              QStringLiteral("editor"), QStringLiteral("moderator"),
                              QStringLiteral("admin")};

// A pseudo-random stride over the token table that hits varied hash buckets (rather than
// re-probing one hot bucket), without needing a RNG. 100003 is prime, so for any table size
// this visits every index before repeating.
constexpr qsizetype kStride{100003};

struct Scalar
{
    QString name;
    QString unit;
    double value{0.0};

    QJsonObject toJson() const
    {
        return QJsonObject{{QStringLiteral("name"), name},
                           {QStringLiteral("unit"), unit},
                           {QStringLiteral("value"), value}};
    }
};

// Fill the manager to `count` live sessions, cycling scopes from the vocabulary, and return
// the credentials so the measured loops can look real sessions up. Also reports the average
// createSession cost (token mint + insert) as it goes.
QList<QByteArray> populate(SessionManager &manager, int count, double &createNsPerOp)
{
    QList<QByteArray> tokens;
    tokens.reserve(count);
    QElapsedTimer clock;
    clock.start();
    for (int i{0}; i < count; ++i) {
        tokens.append(manager.createSession(kScopeOrder.at(i % kScopeOrder.size())));
    }
    createNsPerOp = count > 0 ? double(clock.nsecsElapsed()) / count : 0.0;
    return tokens;
}

// Build a pool of user Callers over a sample of the live tokens, so the hasScope loop varies
// the session it checks (varied buckets) without allocating per iteration.
QList<Caller *> buildCallerPool(SessionManager &manager, const QList<QByteArray> &tokens,
                                int poolSize, bool hierarchical, QObject *parent)
{
    QList<Caller *> pool;
    const int size{qMin(poolSize, int(tokens.size()))};
    pool.reserve(size);
    for (int i{0}; i < size; ++i) {
        const qsizetype index{(qsizetype(i) * kStride) % tokens.size()};
        Caller *caller{Caller::forUser(QString{}, &manager, tokens.at(index), nullptr, parent)};
        caller->setScopeOrder(hierarchical ? kScopeOrder : QStringList{}, hierarchical);
        pool.append(caller);
    }
    return pool;
}

double lookupNsPerOp(SessionManager &manager, const QList<QByteArray> &tokens, int iterations)
{
    qsizetype index{0};
    quint64 sink{0};
    QElapsedTimer clock;
    clock.start();
    for (int i{0}; i < iterations; ++i) {
        index = (index + kStride) % tokens.size();
        const SynQt::SessionRecord *record{manager.lookup(tokens.at(index))};
        sink += record != nullptr ? 1u : 0u;  // defeat dead-code elimination
    }
    const double ns{double(clock.nsecsElapsed()) / iterations};
    if (sink == 0) {
        qWarning("bench-sessions: every lookup missed (unexpected)");
    }
    return ns;
}

double lookupMissNsPerOp(SessionManager &manager, int iterations)
{
    quint64 sink{0};
    QElapsedTimer clock;
    clock.start();
    for (int i{0}; i < iterations; ++i) {
        // A credential shaped like a real token but never issued: the rejection path.
        const QByteArray absent{QByteArrayLiteral("absent-") + QByteArray::number(i)};
        sink += manager.lookup(absent) != nullptr ? 1u : 0u;
    }
    const double ns{double(clock.nsecsElapsed()) / iterations};
    if (sink != 0) {
        qWarning("bench-sessions: an absent credential matched (unexpected)");
    }
    return ns;
}

double hasScopeNsPerOp(const QList<Caller *> &pool, int iterations)
{
    if (pool.isEmpty()) {
        return 0.0;
    }
    qsizetype index{0};
    quint64 sink{0};
    QElapsedTimer clock;
    clock.start();
    for (int i{0}; i < iterations; ++i) {
        index = (index + 1) % pool.size();
        sink += pool.at(index)->hasScope(QStringLiteral("editor")) ? 1u : 0u;
    }
    const double ns{double(clock.nsecsElapsed()) / iterations};
    if (sink == quint64(-1)) {
        qWarning("impossible");  // keep sink live
    }
    return ns;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption iterationsOption{QStringLiteral("iterations"),
        QStringLiteral("Operations per throughput measurement."), QStringLiteral("n"),
        QStringLiteral("500000")};
    const QCommandLineOption sizesOption{QStringLiteral("sizes"),
        QStringLiteral("Comma-separated live-session counts to sweep."), QStringLiteral("list"),
        QStringLiteral("1000,10000,100000")};
    const QCommandLineOption poolOption{QStringLiteral("caller-pool"),
        QStringLiteral("Caller instances for the hasScope sweep."), QStringLiteral("n"),
        QStringLiteral("2000")};
    const QCommandLineOption outOption{QStringLiteral("out"),
        QStringLiteral("JSON baseline output path."), QStringLiteral("file")};
    parser.addOptions({iterationsOption, sizesOption, poolOption, outOption});
    parser.process(app);

    const int iterations{parser.value(iterationsOption).toInt()};
    const int poolSize{parser.value(poolOption).toInt()};
    QList<int> sizes;
    const QStringList sizeTokens{parser.value(sizesOption).split(QLatin1Char(','),
                                                                 Qt::SkipEmptyParts)};
    for (const QString &token : sizeTokens) {
        sizes.append(token.trimmed().toInt());
    }

    QTextStream out{stdout};
    out << "SynQt sessions baseline (SynQt::SessionManager + Caller.hasScope)" << Qt::endl;
    out << "Qt " << qVersion() << " on " << QSysInfo::prettyProductName() << " ("
        << QSysInfo::currentCpuArchitecture() << ")" << Qt::endl;
    out << "iterations/measurement=" << iterations << " caller-pool=" << poolSize << Qt::endl
        << Qt::endl;
    out << qSetFieldWidth(12) << Qt::left << "sessions" << qSetFieldWidth(0)
        << "  lookup_hit  lookup_miss  hasScope_set  hasScope_hier  create   snapshot"
        << Qt::endl;
    out << "            (all ns/op except snapshot in ms)" << Qt::endl;

    QJsonArray sweepJson;
    for (const int count : sizes) {
        SessionManager manager{QStringLiteral("anonymous"), 60};
        double createNs{0.0};
        const QList<QByteArray> tokens{populate(manager, count, createNs)};

        // A fresh owner for the caller pools, destroyed with each sweep step.
        QObject callerOwner;
        const QList<Caller *> setPool{
            buildCallerPool(manager, tokens, poolSize, false, &callerOwner)};
        const QList<Caller *> hierPool{
            buildCallerPool(manager, tokens, poolSize, true, &callerOwner)};

        const double lookupHit{lookupNsPerOp(manager, tokens, iterations)};
        const double lookupMiss{lookupMissNsPerOp(manager, iterations)};
        const double scopeSet{hasScopeNsPerOp(setPool, iterations)};
        const double scopeHier{hasScopeNsPerOp(hierPool, iterations)};

        QElapsedTimer snapClock;
        snapClock.start();
        const QVariantList snap = manager.snapshot();  // '=' not '{}': brace-init would wrap
        const double snapshotMs{double(snapClock.nsecsElapsed()) / 1.0e6};
        if (snap.size() != count) {
            qWarning("bench-sessions: snapshot size %lld != %d", qint64(snap.size()), count);
        }

        out << qSetFieldWidth(12) << Qt::left << count << qSetFieldWidth(0) << Qt::right
            << "  " << qSetFieldWidth(9) << QString::number(lookupHit, 'f', 1)
            << qSetFieldWidth(0) << "  " << qSetFieldWidth(10)
            << QString::number(lookupMiss, 'f', 1) << qSetFieldWidth(0) << "  "
            << qSetFieldWidth(11) << QString::number(scopeSet, 'f', 1) << qSetFieldWidth(0)
            << "  " << qSetFieldWidth(12) << QString::number(scopeHier, 'f', 1)
            << qSetFieldWidth(0) << "  " << qSetFieldWidth(6)
            << QString::number(createNs, 'f', 0) << qSetFieldWidth(0) << "  "
            << QString::number(snapshotMs, 'f', 3) << Qt::endl;

        sweepJson.append(QJsonObject{
            {QStringLiteral("sessions"), count},
            {QStringLiteral("lookup_hit_ns"), lookupHit},
            {QStringLiteral("lookup_miss_ns"), lookupMiss},
            {QStringLiteral("hasscope_set_ns"), scopeSet},
            {QStringLiteral("hasscope_hier_ns"), scopeHier},
            {QStringLiteral("create_ns"), createNs},
            {QStringLiteral("snapshot_ms"), snapshotMs}});
    }

    QJsonObject root;
    root.insert(QStringLiteral("benchmark"), QStringLiteral("sessions"));
    root.insert(QStringLiteral("path"), QStringLiteral("sessionmanager-lookup-and-scope-check"));
    root.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("host"), QSysInfo::prettyProductName());
    root.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    root.insert(QStringLiteral("recorded"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("iterations"), iterations);
    root.insert(QStringLiteral("sweep"), sweepJson);

    const QString outPath{parser.value(outOption)};
    if (!outPath.isEmpty()) {
        QFile file{outPath};
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));
            file.close();
            out << Qt::endl << "wrote baseline " << QDir{outPath}.absolutePath() << Qt::endl;
        } else {
            qWarning("bench-sessions: cannot write %s", qPrintable(outPath));
        }
    }
    return 0;
}
