// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_ENTITYTEST_H
#define SYNQT_ENTITYTEST_H

#include <QtCore/qmutex.h>
#include <QtCore/qobject.h>
#include <QtCore/qstringlist.h>
#include <QtCore/qurl.h>
#include <QtCore/qvariant.h>

#include <memory>

QT_BEGIN_NAMESPACE
class QQmlContext;
class QQmlEngine;
QT_END_NAMESPACE

namespace SynQt {

class Cache;
class Caller;
class ConsumerBase;
class Db;
class Docs;
class ICacheProvider;
class IDocumentProvider;
class IPersistenceProvider;
class Jobs;
class Log;
class SessionManager;

/// The QML type `EntityTest`, in the import `SynQt.Test`: an owned connect point's Source,
/// loaded on its own, with a caller the test chooses.
///
/// \code
/// EntityTest {
///     id: harness
///     source: "../web/edge/Edge.qml"
///
///     function init() { harness.load() }
///
///     function test_a_lower_bid_is_refused() {
///         harness.callerIsUser("user");
///         harness.subject.placeBid(50);
///         compare(harness.subject.highBid, 100);
///     }
/// }
/// \endcode
///
/// The Caller is real, minted through the runtime's own factory. Only the engines are
/// substituted: the type helpers use in-memory providers, so a test needs no database,
/// server or certificates. The class ships in a library and an import a production entity
/// never uses.
class EntityTest : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QObject *subject READ subject NOTIFY subjectChanged)
    Q_PROPERTY(QString schema READ schema WRITE setSchema NOTIFY schemaChanged)
    Q_PROPERTY(QString contract READ contract WRITE setContract NOTIFY contractChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY subjectChanged)

public:
    explicit EntityTest(QObject *parent = nullptr);
    ~EntityTest() override;

    QUrl source() const;
    void setSource(const QUrl &source);

    QObject *subject() const;

    QString schema() const;
    void setSchema(const QString &schema);

    /// The contract name that selects the typed Caller carrying the emit<Signal> sugar.
    /// Derived from the Source type (`AuctionSource` gives `Auction`) unless set.
    QString contract() const;
    void setContract(const QString &contract);

    QString errorString() const;

    /// Build the Source afresh, discarding any state a previous test left in it. Call it
    /// from `init()` so each test function starts from the same place. Returns false and
    /// fills errorString when the QML did not load.
    Q_INVOKABLE bool load();

    /// Who calls the next slot. `identity` is the normalized identity object (`sub`,
    /// `login`, `name`, `email`). An empty one is an anonymous visitor.
    Q_INVOKABLE void callerIsUser(const QString &scope,
                                  const QVariantMap &identity = QVariantMap());
    /// A calling entity. `verified` false is the opt-in local socket case, where the name
    /// is trusted by colocation. Pass it to prove a slot refuses that.
    Q_INVOKABLE void callerIsEntity(const QString &entityName, bool verified = true);
    /// No consumer in the call, as when the owner mutates its own state on a timer.
    Q_INVOKABLE void callerIsNobody();

    /// Call `slot` the way a consumer does: through the generated slot in front of the QML
    /// function, so the member's `<scope>` gate and the export's bounds run before the
    /// function is reached. Returns what the slot returns: the return type's default when
    /// the call was refused, undefined for a slot that returns nothing or does not exist. A
    /// refusal is a warning, as on the wire.
    Q_INVOKABLE QVariant call(const QString &slot,
                              const QVariantList &arguments = QVariantList());

    /// The project's scope vocabulary, so hasScope answers the way the running system
    /// would. Defaults to SynQt's own order, hierarchical.
    Q_INVOKABLE void setScopeOrder(const QStringList &order, bool hierarchical = true);

    /// Read the in-memory database directly, to assert on what a slot wrote rather than on
    /// what it returned.
    Q_INVOKABLE QVariantList dbQuery(const QString &sql,
                                     const QVariantList &params = QVariantList());
    /// Read the in-memory cache directly.
    Q_INVOKABLE QVariant cacheValue(const QString &key);

    /// What the entity recorded while this test ran, oldest first: one map per event with
    /// `severity` and `category` (wire numbers), `severityName` and `categoryName`, `message` and
    /// `attributes`. Drained on each `load()`, so one test never reads another's events.
    Q_INVOKABLE QVariantList recorded() const;

signals:
    void sourceChanged();
    void subjectChanged();
    void schemaChanged();
    void contractChanged();

private:
    enum class CallerKind { Nobody, User, Entity };

    void rebuildCaller();
    void startRecording();
    bool resetEngines();
    QString derivedContract() const;

    QUrl m_source;
    QString m_schema;
    QString m_contract;
    QString m_errorString;
    QObject *m_subject{nullptr};
    QList<ConsumerBase *> m_neighbours;
    QQmlContext *m_context{nullptr};
    QQmlEngine *m_engine{nullptr};

    CallerKind m_callerKind{CallerKind::Nobody};
    QString m_callerScope;
    QVariantMap m_callerIdentity;
    QString m_callerEntity;
    bool m_callerVerified{true};
    QStringList m_scopeOrder;
    bool m_hierarchical{true};
    Caller *m_caller{nullptr};
    SessionManager *m_sessions{nullptr};

    std::unique_ptr<IPersistenceProvider> m_persistence;
    std::unique_ptr<ICacheProvider> m_cache;
    std::unique_ptr<IDocumentProvider> m_document;
    mutable QMutex m_recordedMutex;
    QVariantList m_recorded;
    bool m_recording{false};
    Log *m_log{nullptr};
    Db *m_db{nullptr};
    Cache *m_cacheHelper{nullptr};
    Docs *m_docs{nullptr};
    Jobs *m_jobs{nullptr};
};

/// Register `EntityTest` under the import `SynQt.Test`. The generated test main calls it;
/// nothing else does, which is what keeps the harness out of a running entity.
void registerTestTypes();

/// Declare that the entity owning `ownerContract` consumes `contract` at `point`, read in
/// QML as `accessor`. The generated test main declares the whole topology through this.
/// load() installs each declared point as the runtime does before the link opens: the
/// accessor exists and is never ready, so a call through it reaches nothing.
void declareConsumedPoint(const QString &ownerContract, const QString &accessor,
                          const QString &contract, const QString &point);

} // namespace SynQt

#endif // SYNQT_ENTITYTEST_H
