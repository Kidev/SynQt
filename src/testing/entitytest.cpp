// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "entitytest.h"

#include "cache.h"
#include "cachefactory.h"
#include "caller.h"
#include "consumerbase.h"
#include "consumerfactory.h"
#include "db.h"
#include "docs.h"
#include "documentfactory.h"
#include "icacheprovider.h"
#include "idocumentprovider.h"
#include "ipersistenceprovider.h"
#include "jobs.h"
#include "log.h"
#include "persistencefactory.h"
#include "providerconfig.h"
#include "sessionmanager.h"
#include "sourcefactory.h"
#include "tracer.h"

#include <QtQml/qqmlcomponent.h>
#include <QtQml/qqmlcontext.h>
#include <QtQml/qqmlengine.h>
#include <QtQml/qqmlinfo.h>

#include <QtTest/qtestcase.h>

#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qhash.h>
#include <QtCore/qmetaobject.h>
#include <QtCore/qregularexpression.h>

namespace SynQt {

namespace {

/// The scope vocabulary `synqt new` writes, so a test behaves like the app by default.
const QStringList &defaultScopeOrder()
{
    static const QStringList order{QStringLiteral("anonymous"), QStringLiteral("user"),
                                   QStringLiteral("moderator"), QStringLiteral("admin")};
    return order;
}

/// Split a schema file into migrate() statements as the runtime does (topologywriter.py):
/// `--` comments dropped line by line, then a statement at every semicolon.
QStringList schemaSteps(const QString &text)
{
    QStringList code;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const qsizetype comment{line.indexOf(QLatin1String("--"))};
        code.append(comment < 0 ? line : line.left(comment));
    }
    QStringList steps;
    for (const QString &piece : code.join(QLatin1Char('\n')).split(QLatin1Char(';'))) {
        const QString trimmed{piece.trimmed()};
        if (!trimmed.isEmpty()) {
            steps.append(trimmed);
        }
    }
    return steps;
}

/// A connect point an owner consumes, as the generated test main declares it.
struct ConsumedPoint
{
    QString accessor;
    QString contract;
    QString point;
};

/// What each owner consumes, by the owner's contract.
QHash<QString, QList<ConsumedPoint>> &consumedPoints()
{
    static QHash<QString, QList<ConsumedPoint>> points;
    return points;
}

} // namespace

EntityTest::EntityTest(QObject *parent)
    : QObject{parent},
      m_scopeOrder{defaultScopeOrder()}
{
    // One session store for the harness, with a scaffolded project's default scope, so an
    // unauthenticated caller is anonymous.
    m_sessions = new SessionManager{QStringLiteral("anonymous"), 60, this};
    m_log = new Log{this};
}

EntityTest::~EntityTest()
{
    // The sink points at a member of this object, so it has to go before this object does.
    // The writer thread calls a copy of the sink outside the tracer's lock, and the flush
    // waits for a delivery already under way to return.
    Tracer::instance()->setSink(Tracer::Sink{});
    Tracer::instance()->flush();
    Tracer::instance()->setEnabled(false);
}

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
    // The contract name selects the typed Caller with emit<Signal>, and it survives at run
    // time only in the generated C++ type name. A QML Source is a subclass named like
    // `Ledger_QMLTYPE_0`, so walk up to `LedgerSourceHelper` and take the prefix.
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
    // The Caller carries the Source it emits through, so it is built after the Source and
    // never outlives it: here, for both a caller set before load() and a load() under an
    // existing caller.
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
    // Bound to the Source the way a transport binds it, so the generated gates on props,
    // signals and slots answer this caller.
    SourceFactory::bindCaller(m_subject, m_caller);
    // A null Caller means no caller: read outside a call it gives nothing, as on the
    // entity.
    m_context->setContextProperty(QStringLiteral("Caller"), m_caller);
    m_context->setContextProperty(QStringLiteral("Client"),
                                  m_callerKind == CallerKind::User ? m_caller : nullptr);
}

namespace {

/// `value` as the parameter type a generated slot declares. A record crosses QML as an
/// object, so it is written onto the gadget property by property.
bool convertArgument(QVariant *value, QMetaType type)
{
    if (value->metaType() == type) {
        return true;
    }
    const QMetaObject *gadget{type.metaObject()};
    if (gadget != nullptr && (type.flags() & QMetaType::IsGadget)
        && value->canConvert<QVariantMap>()) {
        const QVariantMap fields{value->toMap()};
        QVariant record{type};
        for (int index{0}; index < gadget->propertyCount(); ++index) {
            const QMetaProperty property{gadget->property(index)};
            const QString name{QString::fromUtf8(property.name())};
            if (fields.contains(name)) {
                QVariant field{fields.value(name)};
                if (!convertArgument(&field, property.metaType())) {
                    return false;
                }
                property.writeOnGadget(record.data(), field);
            }
        }
        *value = record;
        return true;
    }
    return value->convert(type);
}

} // namespace

QVariant EntityTest::call(const QString &slot, const QVariantList &arguments)
{
    if (m_subject == nullptr) {
        qWarning("SynQt: call('%s') before load()", qUtf8Printable(slot));
        return QVariant{};
    }
    // The generated slot: declared by repc on the Source class and implemented by the
    // SourceHelper over it. The QML function of the same name sits on the type above the
    // helper, so a lookup by name from the top would find the function and skip the checks.
    // Searched within the helper's range, which takes in every class below it, and invoked
    // virtually, so the helper's implementation is what runs.
    const QMetaObject *helper{m_subject->metaObject()};
    while (helper != nullptr
           && !QByteArray{helper->className()}.endsWith(QByteArrayLiteral("SourceHelper"))) {
        helper = helper->superClass();
    }
    QMetaMethod method;
    const QByteArray name{slot.toUtf8()};
    for (int index{helper != nullptr ? helper->methodCount() - 1 : -1}; index >= 0; --index) {
        const QMetaMethod candidate{helper->method(index)};
        if (candidate.methodType() != QMetaMethod::Slot || candidate.name() != name) {
            continue;
        }
        // A point a service consumes carries the session the calling entity acts for as a
        // last parameter, which a test does not write. The harness forwards none.
        const qsizetype count{candidate.parameterCount()};
        const bool forwardsSession{
            count == arguments.size() + 1
            && candidate.parameterNames().constLast() == QByteArrayLiteral("synqtSession")};
        if (count == arguments.size() || forwardsSession) {
            method = candidate;
            break;
        }
    }
    if (!method.isValid()) {
        qWarning("SynQt: the contract has no slot %s taking %lld argument(s)",
                 qUtf8Printable(slot), static_cast<long long>(arguments.size()));
        return QVariant{};
    }

    // `=`, not brace-init: QVariantList{aList} wraps the list as a single element.
    QVariantList converted = arguments;
    if (method.parameterCount() > arguments.size()) {
        converted.append(QVariantMap{});
    }
    QList<void *> argv{nullptr};
    for (int index{0}; index < method.parameterCount(); ++index) {
        if (!convertArgument(&converted[index], method.parameterMetaType(index))) {
            qWarning("SynQt: argument %d of %s cannot be a %s", index + 1,
                     qUtf8Printable(slot), method.parameterMetaType(index).name());
            return QVariant{};
        }
        argv.append(converted[index].data());
    }
    const bool returns{method.returnMetaType() != QMetaType::fromType<void>()};
    QVariant result{returns ? QVariant{method.returnMetaType()} : QVariant{}};
    if (returns) {
        argv[0] = result.data();
    }
    QMetaObject::metacall(m_subject, QMetaObject::InvokeMetaMethod, method.methodIndex(),
                          argv.data());
    return result;
}

void EntityTest::startRecording()
{
    if (m_recording) {
        return;
    }
    m_recording = true;

    // The real trace pipeline, enabled for the entity under test, so `Log.info(...)` can be
    // asserted. Delivered on the writer thread, so the list is guarded.
    Tracer::instance()->setEntity(QStringLiteral("test"));
    Tracer::instance()->setEnabled(true);
    Tracer::instance()->setBatch(1, 20);
    Tracer::instance()->setSink([this](const QList<TraceEvent> &batch) {
        QMutexLocker locker{&m_recordedMutex};
        for (const TraceEvent &event : batch) {
            QVariantMap value{event.toVariant()};
            // The names beside the numbers, so a test does not assert `category === 5`.
            value.insert(QStringLiteral("severityName"), severityName(event.severity));
            value.insert(QStringLiteral("categoryName"), categoryName(event.category));
            m_recorded.append(value);
        }
    });
}

bool EntityTest::resetEngines()
{
    // The helpers hold the providers by raw pointer, so they go first. Deleting Jobs stops
    // every timer the previous Source started.
    delete m_db;
    m_db = nullptr;
    delete m_cacheHelper;
    m_cacheHelper = nullptr;
    delete m_docs;
    m_docs = nullptr;
    delete m_jobs;
    m_jobs = nullptr;
    m_persistence.reset();
    m_cache.reset();
    m_document.reset();

    // Every helper an entity could have, not just its type's, so the harness needs no type
    // setting.
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

    if (m_schema.isEmpty()) {
        return true;
    }
    if (m_persistence == nullptr) {
        m_errorString = error;
        return false;
    }
    const QUrl schemaUrl{qmlContext(this)->resolvedUrl(QUrl{m_schema})};
    QFile file{schemaUrl.isLocalFile() ? schemaUrl.toLocalFile() : m_schema};
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_errorString = QStringLiteral("cannot read schema '%1'").arg(m_schema);
        return false;
    }
    const QStringList steps{schemaSteps(QString::fromUtf8(file.readAll()))};
    if (!m_persistence->migrate(steps, &error)) {
        m_errorString = error;
        return false;
    }
    return true;
}

QVariantList EntityTest::recorded() const
{
    Tracer::instance()->flush();
    QMutexLocker locker{&m_recordedMutex};
    return m_recorded;
}

bool EntityTest::load()
{
    m_errorString.clear();
    delete m_caller;
    m_caller = nullptr;
    delete m_subject;
    m_subject = nullptr;
    qDeleteAll(m_neighbours);
    m_neighbours.clear();
    delete m_context;
    m_context = nullptr;

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

    startRecording();
    {
        // Drained per load, so one test never reads what an earlier one said.
        Tracer::instance()->flush();
        QMutexLocker locker{&m_recordedMutex};
        m_recorded.clear();
    }

    // Fresh engines behind every helper, so results do not depend on test order.
    if (!resetEngines()) {
        emit subjectChanged();
        return false;
    }

    m_context = new QQmlContext{m_engine->rootContext(), this};
    m_context->setContextProperty(QStringLiteral("Db"), m_db);
    m_context->setContextProperty(QStringLiteral("Cache"), m_cacheHelper);
    m_context->setContextProperty(QStringLiteral("Docs"), m_docs);
    m_context->setContextProperty(QStringLiteral("Jobs"), m_jobs);
    m_context->setContextProperty(QStringLiteral("Log"), m_log);
    m_context->setContextProperty(QStringLiteral("Caller"), nullptr);
    m_context->setContextProperty(QStringLiteral("Client"), nullptr);

    // Each entity this one consumes, as the runtime installs it before the link opens: the
    // accessor and its `<Owner>.on<Signal>` handlers resolve, `ready` stays false, and a
    // call through it is dropped with a warning. That warning fails the running test, so a
    // slot that reaches a neighbour cannot pass over a call that never happened.
    const QUrl resolved{qmlContext(this)->resolvedUrl(m_source)};
    const QString owner{m_contract.isEmpty() ? QFileInfo{resolved.path()}.completeBaseName()
                                             : m_contract};
    for (const ConsumedPoint &consumed : consumedPoints().value(owner)) {
        ConsumerBase *facade{makeConsumer(consumed.contract)};
        if (facade == nullptr) {
            continue;
        }
        facade->setPoint(consumed.point);
        facade->setJsEngine(m_engine);
        facade->setParent(this);
        m_neighbours.append(facade);
        m_context->setContextProperty(consumed.accessor, facade);
    }
    QTest::failOnWarning(QRegularExpression{
        QStringLiteral("^SynQt: the '[^']+' connect point is not available, so ")});

    QQmlComponent component{m_engine, resolved, this};
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

void declareConsumedPoint(const QString &ownerContract, const QString &accessor,
                          const QString &contract, const QString &point)
{
    consumedPoints()[ownerContract].append(ConsumedPoint{accessor, contract, point});
}

} // namespace SynQt
