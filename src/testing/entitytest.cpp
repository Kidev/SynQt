// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "entitytest.h"

#include "cache.h"
#include "cachefactory.h"
#include "caller.h"
#include "db.h"
#include "docs.h"
#include "documentfactory.h"
#include "icacheprovider.h"
#include "idocumentprovider.h"
#include "ipersistenceprovider.h"
#include "jobs.h"
#include "persistencefactory.h"
#include "providerconfig.h"
#include "sessionmanager.h"

#include <QtQml/qqmlcomponent.h>
#include <QtQml/qqmlcontext.h>
#include <QtQml/qqmlengine.h>
#include <QtQml/qqmlinfo.h>

#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qmetaobject.h>

namespace SynQt {

namespace {

/// The scope vocabulary a project gets from `synqt new`. A test that never says otherwise
/// should behave like the app it is testing, not like an empty configuration.
const QStringList &defaultScopeOrder()
{
    static const QStringList order{QStringLiteral("anonymous"), QStringLiteral("user"),
                                   QStringLiteral("moderator"), QStringLiteral("admin")};
    return order;
}

/// Split a schema file into the statements migrate() applies. The same naive split the
/// runtime uses: statements are separated by semicolons at the end of a line.
QStringList schemaSteps(const QString &text)
{
    QStringList steps;
    for (const QString &piece : text.split(QLatin1Char(';'))) {
        const QString trimmed{piece.trimmed()};
        if (!trimmed.isEmpty()) {
            steps.append(trimmed);
        }
    }
    return steps;
}

} // namespace

EntityTest::EntityTest(QObject *parent)
    : QObject{parent},
      m_scopeOrder{defaultScopeOrder()}
{
    // One session store for the harness, with the same default scope a scaffolded project
    // uses, so an unauthenticated caller is anonymous here too.
    m_sessions = new SessionManager{QStringLiteral("anonymous"), 60, this};
}

EntityTest::~EntityTest() = default;

QUrl EntityTest::source() const
{
    return m_source;
}

void EntityTest::setSource(const QUrl &source)
{
    if (m_source == source) {
        return;
    }
    m_source = source;
    emit sourceChanged();
}

QObject *EntityTest::subject() const
{
    return m_subject;
}

QString EntityTest::schema() const
{
    return m_schema;
}

void EntityTest::setSchema(const QString &schema)
{
    if (m_schema == schema) {
        return;
    }
    m_schema = schema;
    emit schemaChanged();
}

QString EntityTest::contract() const
{
    return m_contract.isEmpty() ? derivedContract() : m_contract;
}

void EntityTest::setContract(const QString &contract)
{
    if (m_contract == contract) {
        return;
    }
    m_contract = contract;
    emit contractChanged();
    rebuildCaller();
}

QString EntityTest::errorString() const
{
    return m_errorString;
}

QString EntityTest::derivedContract() const
{
    if (m_subject == nullptr) {
        return QString{};
    }
    // The contract name selects the typed Caller that carries emit<Signal>, and the only
    // place it survives at run time is the generated C++ type's name. The subject is not
    // that type: a Source written in QML is a QML-defined subclass whose own class name is
    // `Ledger_QMLTYPE_0`, which says nothing. So walk up to the generated base,
    // `LedgerSourceHelper`, and take what precedes it.
    for (const QMetaObject *type{m_subject->metaObject()}; type != nullptr;
         type = type->superClass()) {
        const QString className{QString::fromUtf8(type->className())};
        if (className.endsWith(QLatin1String("SourceHelper"))) {
            return className.chopped(QLatin1String("SourceHelper").size());
        }
    }
    return QString{};
}

void EntityTest::setScopeOrder(const QStringList &order, bool hierarchical)
{
    m_scopeOrder = order.isEmpty() ? defaultScopeOrder() : order;
    m_hierarchical = hierarchical;
    rebuildCaller();
}

void EntityTest::callerIsUser(const QString &scope, const QVariantMap &identity)
{
    m_callerKind = CallerKind::User;
    m_callerScope = scope;
    m_callerIdentity = identity;
    rebuildCaller();
}

void EntityTest::callerIsEntity(const QString &entityName, bool verified)
{
    m_callerKind = CallerKind::Entity;
    m_callerEntity = entityName;
    m_callerVerified = verified;
    rebuildCaller();
}

void EntityTest::callerIsNobody()
{
    m_callerKind = CallerKind::Nobody;
    rebuildCaller();
}

void EntityTest::rebuildCaller()
{
    // The Caller carries the Source it may emit back through, so it cannot outlive one and
    // cannot be built before one exists. Both directions land here: setting the caller
    // before load(), and load() replacing the subject under an already-chosen caller.
    delete m_caller;
    m_caller = nullptr;
    if (m_subject == nullptr || m_context == nullptr) {
        return;
    }

    switch (m_callerKind) {
    case CallerKind::Nobody:
        break;
    case CallerKind::User: {
        const QByteArray sessionId{m_sessions->createSession(m_callerScope, m_callerIdentity)};
        m_caller = Caller::forUser(contract(), m_sessions, sessionId, m_subject, this);
        break;
    }
    case CallerKind::Entity:
        m_caller = Caller::forEntity(contract(), m_callerEntity, m_callerVerified,
                                     m_subject, this);
        break;
    }

    if (m_caller != nullptr) {
        m_caller->setScopeOrder(m_scopeOrder, m_hierarchical);
    }
    // A null Caller is the honest representation of no caller: a slot that reads it
    // outside a call gets nothing, exactly as it would on the entity.
    m_context->setContextProperty(QStringLiteral("Caller"), m_caller);
    m_context->setContextProperty(QStringLiteral("Client"),
                                  m_callerKind == CallerKind::User ? m_caller : nullptr);
}

void EntityTest::buildHelpers()
{
    if (m_db != nullptr) {
        return;   // built once per harness; load() resets their contents, not their wiring
    }

    // Every helper an entity could have, rather than the ones its blueprint would give it.
    // A test harness that guessed the blueprint would be one more thing to configure, and
    // guessing wrong would surface as an undefined name rather than as a clear failure.
    ProviderConfig persistenceConfig;
    persistenceConfig.name = QStringLiteral("sqlite");
    persistenceConfig.file = QStringLiteral(":memory:");
    persistenceConfig.journalMode = QStringLiteral("memory");
    persistenceConfig.release = false;
    QString error;
    m_persistence = makePersistenceProvider(persistenceConfig, &error);
    if (m_persistence != nullptr && !m_persistence->connect(&error)) {
        m_persistence.reset();
    }

    ProviderConfig memoryConfig;
    memoryConfig.name = QStringLiteral("memory");
    memoryConfig.release = false;
    m_cache = makeCacheProvider(memoryConfig, &error);
    if (m_cache != nullptr) {
        m_cache->connect(&error);
    }
    m_document = makeDocumentProvider(memoryConfig, &error);
    if (m_document != nullptr) {
        m_document->connect(&error);
    }

    m_db = new Db{m_persistence.get(), this};
    m_cacheHelper = new Cache{m_cache.get(), this};
    m_docs = new Docs{m_document.get(), this};
    m_jobs = new Jobs{1000, this};
}

bool EntityTest::load()
{
    m_errorString.clear();
    delete m_caller;
    m_caller = nullptr;
    delete m_subject;
    m_subject = nullptr;

    if (m_source.isEmpty()) {
        m_errorString = QStringLiteral("EntityTest.source is not set");
        emit subjectChanged();
        return false;
    }

    m_engine = qmlEngine(this);
    if (m_engine == nullptr) {
        m_errorString = QStringLiteral("EntityTest must be created from QML");
        emit subjectChanged();
        return false;
    }

    buildHelpers();

    // Reset the state, not the wiring: a fresh Source over a database still holding the
    // previous test's rows would pass or fail depending on test order.
    if (m_persistence != nullptr) {
        QString error;
        m_persistence->disconnect();
        if (!m_persistence->connect(&error)) {
            m_errorString = error;
            emit subjectChanged();
            return false;
        }
        if (!m_schema.isEmpty()) {
            const QUrl schemaUrl{qmlContext(this)->resolvedUrl(QUrl{m_schema})};
            QFile file{schemaUrl.isLocalFile() ? schemaUrl.toLocalFile() : m_schema};
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                m_errorString = QStringLiteral("cannot read schema '%1'").arg(m_schema);
                emit subjectChanged();
                return false;
            }
            const QStringList steps{schemaSteps(QString::fromUtf8(file.readAll()))};
            if (!m_persistence->migrate(steps, &error)) {
                m_errorString = error;
                emit subjectChanged();
                return false;
            }
        }
    }

    delete m_context;
    m_context = new QQmlContext{m_engine->rootContext(), this};
    m_context->setContextProperty(QStringLiteral("Db"), m_db);
    m_context->setContextProperty(QStringLiteral("Cache"), m_cacheHelper);
    m_context->setContextProperty(QStringLiteral("Docs"), m_docs);
    m_context->setContextProperty(QStringLiteral("Jobs"), m_jobs);
    m_context->setContextProperty(QStringLiteral("Caller"), nullptr);
    m_context->setContextProperty(QStringLiteral("Client"), nullptr);

    QQmlComponent component{m_engine, qmlContext(this)->resolvedUrl(m_source), this};
    if (component.isError()) {
        m_errorString = component.errorString().trimmed();
        emit subjectChanged();
        return false;
    }
    m_subject = component.create(m_context);
    if (m_subject == nullptr) {
        m_errorString = component.errorString().trimmed();
        emit subjectChanged();
        return false;
    }
    m_subject->setParent(this);

    rebuildCaller();
    emit subjectChanged();
    return true;
}

QVariantList EntityTest::dbQuery(const QString &sql, const QVariantList &params)
{
    if (m_persistence == nullptr) {
        return QVariantList{};
    }
    return m_persistence->query(sql, params).rows;
}

QVariant EntityTest::cacheValue(const QString &key)
{
    if (m_cache == nullptr) {
        return QVariant{};
    }
    return m_cache->get(key);
}

void registerTestTypes()
{
    qmlRegisterType<EntityTest>("SynQt.Test", 1, 0, "EntityTest");
}

} // namespace SynQt
