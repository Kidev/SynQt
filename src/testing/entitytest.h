// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_ENTITYTEST_H
#define SYNQT_ENTITYTEST_H

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
class Db;
class Docs;
class ICacheProvider;
class IDocumentProvider;
class IPersistenceProvider;
class Jobs;
class SessionManager;

/// The QML type `EntityTest`, in the import `SynQt.Test`: an owned connect point's Source,
/// loaded on its own, with a caller the test chooses.
///
/// An owner slot is where authorization lives, so it is the thing most worth testing, and
/// until now testing one meant writing C++ against Caller::forUser. This is the same
/// machinery driven from QML, so a slot written in QML is tested in QML:
///
/// \code
/// EntityTest {
///     id: harness
///     source: "../web/Auction.qml"
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
/// It is the real Caller, minted through the same factory the runtime uses, so a slot
/// cannot pass here and fail in production because the test stubbed the check. What is
/// substituted is only what an engine would otherwise be: the blueprint helpers are backed
/// by in-memory providers, so a test needs no database, no server, and no certificates.
///
/// This class ships in a library a production entity never links and registers into an
/// import a production entity never writes, so nothing here can widen the runtime.
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
    /// `login`, `name`, `email`); an empty one is an anonymous visitor.
    Q_INVOKABLE void callerIsUser(const QString &scope,
                                  const QVariantMap &identity = QVariantMap());
    /// A calling entity. `verified` false is the opt-in local socket case, where the name
    /// is trusted by colocation; pass it to prove a slot refuses that.
    Q_INVOKABLE void callerIsEntity(const QString &entityName, bool verified = true);
    /// No consumer in the call, as when the owner mutates its own state on a timer.
    Q_INVOKABLE void callerIsNobody();

    /// The project's scope vocabulary, so hasScope answers the way the running system
    /// would. Defaults to SynQt's own order, hierarchical.
    Q_INVOKABLE void setScopeOrder(const QStringList &order, bool hierarchical = true);

    /// Read the in-memory database directly, to assert on what a slot wrote rather than on
    /// what it returned.
    Q_INVOKABLE QVariantList dbQuery(const QString &sql,
                                     const QVariantList &params = QVariantList());
    /// Read the in-memory cache directly.
    Q_INVOKABLE QVariant cacheValue(const QString &key);

signals:
    void sourceChanged();
    void subjectChanged();
    void schemaChanged();
    void contractChanged();

private:
    enum class CallerKind { Nobody, User, Entity };

    void rebuildCaller();
    void buildHelpers();
    QString derivedContract() const;

    QUrl m_source;
    QString m_schema;
    QString m_contract;
    QString m_errorString;
    QObject *m_subject{nullptr};
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
    Db *m_db{nullptr};
    Cache *m_cacheHelper{nullptr};
    Docs *m_docs{nullptr};
    Jobs *m_jobs{nullptr};
};

/// Register `EntityTest` under the import `SynQt.Test`. The generated test main calls it;
/// nothing else does, which is what keeps the harness out of a running entity.
void registerTestTypes();

} // namespace SynQt

#endif // SYNQT_ENTITYTEST_H
