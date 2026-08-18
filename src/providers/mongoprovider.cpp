// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "mongoprovider.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <mongoc/mongoc.h>

#include <utility>

namespace SynQt {

namespace {

// Convert a map to a BSON document through JSON (the driver ships a JSON<->BSON codec).
//
// Returns null when libbson cannot build one. libbson reads a `$` key as extended JSON, so
// `{"$date": "not a date"}` is refused, and the result must be nothing: an empty document
// is a filter that matches every document.
bson_t *bsonFromMap(const QVariantMap &map)
{
    const QByteArray json{
        QJsonDocument{QJsonObject::fromVariantMap(map)}.toJson(QJsonDocument::Compact)};
    bson_error_t error;
    return bson_new_from_json(reinterpret_cast<const uint8_t *>(json.constData()),
                              json.size(), &error);
}

void destroyIfBuilt(bson_t *document)
{
    if (document != nullptr) {
        bson_destroy(document);
    }
}

// Logged once per refused operation, naming the operation and the collection, never the
// values.
void warnUnreadable(const char *operation, const QString &collection)
{
    qWarning("SynQt: Docs.%s on '%s' was refused: its %s cannot be read as a MongoDB "
             "document (a key starting with '$' must be a valid extended JSON value)",
             operation, qUtf8Printable(collection),
             qstrcmp(operation, "insert") == 0 ? "document" : "filter");
}

// A value with every ObjectId in it written as its 24 hex digits, which is the id insert()
// hands back and the kind of id the memory provider keeps. Relaxed extended JSON writes one
// as {"$oid": "..."}.
QVariant withPlainIds(const QVariant &value)
{
    if (value.typeId() == QMetaType::QVariantMap) {
        const QVariantMap map{value.toMap()};
        if (map.size() == 1 && map.contains(QStringLiteral("$oid"))) {
            return map.value(QStringLiteral("$oid")).toString();
        }
        QVariantMap plain;
        for (auto it{map.constBegin()}; it != map.constEnd(); ++it) {
            plain.insert(it.key(), withPlainIds(it.value()));
        }
        return plain;
    }
    if (value.typeId() == QMetaType::QVariantList) {
        QVariantList plain;
        for (const QVariant &item : value.toList()) {
            plain.append(withPlainIds(item));
        }
        return plain;
    }
    return value;
}

QVariantMap mapFromBson(const bson_t *document)
{
    char *json{bson_as_relaxed_extended_json(document, nullptr)};
    if (json == nullptr) {
        return QVariantMap{};
    }
    const QVariantMap map{QJsonDocument::fromJson(QByteArray{json}).object().toVariantMap()};
    bson_free(json);
    return withPlainIds(map).toMap();
}

// A filter whose `_id` is the text of an ObjectId, widened to match either the ObjectId
// insert() minted or a document stored with that text as its own `_id`.
QVariantMap filterWithIds(const QVariantMap &filter)
{
    const QVariant id{filter.value(QStringLiteral("_id"))};
    if (id.typeId() != QMetaType::QString) {
        return filter;
    }
    const QString text{id.toString()};
    if (!bson_oid_is_valid(text.toLatin1().constData(), static_cast<size_t>(text.size()))) {
        return filter;
    }
    QVariantMap widened{filter};
    widened.insert(QStringLiteral("_id"),
                   QVariantMap{{QStringLiteral("$in"),
                                QVariantList{QVariantMap{{QStringLiteral("$oid"), text}},
                                             text}}});
    return widened;
}

} // namespace

MongoDocumentProvider::MongoDocumentProvider(ProviderConfig config)
    : m_config{std::move(config)}
{
}

MongoDocumentProvider::~MongoDocumentProvider()
{
    disconnect();
}

QString MongoDocumentProvider::name() const
{
    return QStringLiteral("mongodb");
}

bool MongoDocumentProvider::refusesInsecure() const
{
    // No loopback exemption, unlike the relational and cache providers: the engine address
    // is inside the connection string, not in `host`, so `isLoopbackHost()` would exempt
    // every deployment. A release entity sets `tls: true` and its URI must agree (checked
    // below), or it does not start.
    return m_config.release && !m_config.tls;
}

bool MongoDocumentProvider::connect(QString *error)
{
    if (refusesInsecure()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "refusing an unverified MongoDB connection in release: enable TLS with a "
                "verified CA in the connection string (see docs/security.md)");
        }
        return false;
    }

    static bool initialized{false};
    if (!initialized) {
        mongoc_init();
        initialized = true;
    }

    bson_error_t bsonError;
    mongoc_uri_t *uri{mongoc_uri_new_with_error(m_config.uri.toUtf8().constData(), &bsonError)};
    if (uri == nullptr) {
        if (error != nullptr) {
            *error = QString::fromUtf8(bsonError.message);  // never contains the password
        }
        return false;
    }
    // Trust the URI, not the flag: `tls: true` beside a URI without `tls=true` would be
    // plaintext, and `tlsInsecure` or `tlsAllowInvalidCertificates` turn off certificate
    // checks inside a TLS URI. Both are refused here.
    if (m_config.tls && !mongoc_uri_get_tls(uri)) {
        mongoc_uri_destroy(uri);
        if (error != nullptr) {
            *error = QStringLiteral(
                "the MongoDB connection string does not enable TLS, but this provider is "
                "configured for it: add tls=true to the uri (see "
                "https://synqt.org/providers/)");
        }
        return false;
    }
    if (m_config.tls
        && (mongoc_uri_get_option_as_bool(uri, MONGOC_URI_TLSINSECURE, false)
            || mongoc_uri_get_option_as_bool(uri, MONGOC_URI_TLSALLOWINVALIDCERTIFICATES,
                                             false)
            || mongoc_uri_get_option_as_bool(uri, MONGOC_URI_TLSALLOWINVALIDHOSTNAMES,
                                             false))) {
        mongoc_uri_destroy(uri);
        if (error != nullptr) {
            *error = QStringLiteral(
                "the MongoDB connection string disables certificate verification "
                "(tlsInsecure / tlsAllowInvalidCertificates / tlsAllowInvalidHostnames); "
                "an unverified TLS link is refused (see https://synqt.org/security/)");
        }
        return false;
    }
    mongoc_client_t *client{mongoc_client_new_from_uri(uri)};
    mongoc_uri_destroy(uri);
    if (client == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("could not create a MongoDB client");
        }
        return false;
    }
    mongoc_client_set_error_api(client, MONGOC_ERROR_API_VERSION_2);
    m_client = client;
    return true;
}

void MongoDocumentProvider::disconnect()
{
    if (m_client != nullptr) {
        mongoc_client_destroy(static_cast<mongoc_client_t *>(m_client));
        m_client = nullptr;
    }
}

bool MongoDocumentProvider::isHealthy() const
{
    return m_client != nullptr;
}

QVariant MongoDocumentProvider::insert(const QString &collection, const QVariantMap &document)
{
    if (m_client == nullptr) {
        return QVariant{};
    }
    bson_t *doc{bsonFromMap(document)};
    if (doc == nullptr) {
        warnUnreadable("insert", collection);
        return QVariant{};
    }
    mongoc_collection_t *coll{mongoc_client_get_collection(
        static_cast<mongoc_client_t *>(m_client), m_config.database.toUtf8().constData(),
        collection.toUtf8().constData())};

    // Mint an id when the caller supplied none, and return whatever id the document carries.
    QVariant insertedId;
    if (!bson_has_field(doc, "_id")) {
        bson_oid_t oid;
        bson_oid_init(&oid, nullptr);
        bson_append_oid(doc, "_id", -1, &oid);
        char buffer[25];
        bson_oid_to_string(&oid, buffer);
        insertedId = QString::fromLatin1(buffer);
    } else {
        insertedId = mapFromBson(doc).value(QStringLiteral("_id"));
    }

    bson_error_t error;
    const bool ok{mongoc_collection_insert_one(coll, doc, nullptr, nullptr, &error)};
    bson_destroy(doc);
    mongoc_collection_destroy(coll);
    return ok ? insertedId : QVariant{};
}

QVariantList MongoDocumentProvider::find(const QString &collection, const QVariantMap &filter,
                                         const QVariantMap &options)
{
    QVariantList rows;
    if (m_client == nullptr) {
        return rows;
    }
    bson_t *query{bsonFromMap(filterWithIds(filter))};
    bson_t *opts{bsonFromMap(options)};
    if (query == nullptr || opts == nullptr) {
        destroyIfBuilt(query);
        destroyIfBuilt(opts);
        warnUnreadable("find", collection);
        return rows;
    }
    mongoc_collection_t *coll{mongoc_client_get_collection(
        static_cast<mongoc_client_t *>(m_client), m_config.database.toUtf8().constData(),
        collection.toUtf8().constData())};
    mongoc_cursor_t *cursor{mongoc_collection_find_with_opts(coll, query, opts, nullptr)};

    const bson_t *document{nullptr};
    while (mongoc_cursor_next(cursor, &document)) {
        rows.append(mapFromBson(document));
    }

    mongoc_cursor_destroy(cursor);
    bson_destroy(opts);
    bson_destroy(query);
    mongoc_collection_destroy(coll);
    return rows;
}

int MongoDocumentProvider::update(const QString &collection, const QVariantMap &filter,
                                  const QVariantMap &change)
{
    if (m_client == nullptr) {
        return 0;
    }
    bson_t *selector{bsonFromMap(filterWithIds(filter))};
    bson_t *update{bsonFromMap(QVariantMap{{QStringLiteral("$set"), change}})};
    if (selector == nullptr || update == nullptr) {
        destroyIfBuilt(selector);
        destroyIfBuilt(update);
        warnUnreadable("update", collection);
        return 0;
    }
    mongoc_collection_t *coll{mongoc_client_get_collection(
        static_cast<mongoc_client_t *>(m_client), m_config.database.toUtf8().constData(),
        collection.toUtf8().constData())};

    bson_t reply;
    bson_error_t error;
    const bool ok{mongoc_collection_update_many(coll, selector, update, nullptr, &reply, &error)};
    int matched{0};
    if (ok) {
        matched = mapFromBson(&reply).value(QStringLiteral("modifiedCount")).toInt();
    }
    bson_destroy(&reply);
    bson_destroy(update);
    bson_destroy(selector);
    mongoc_collection_destroy(coll);
    return matched;
}

int MongoDocumentProvider::remove(const QString &collection, const QVariantMap &filter)
{
    if (m_client == nullptr) {
        return 0;
    }
    bson_t *selector{bsonFromMap(filterWithIds(filter))};
    if (selector == nullptr) {
        warnUnreadable("remove", collection);
        return 0;
    }
    mongoc_collection_t *coll{mongoc_client_get_collection(
        static_cast<mongoc_client_t *>(m_client), m_config.database.toUtf8().constData(),
        collection.toUtf8().constData())};

    bson_t reply;
    bson_error_t error;
    const bool ok{mongoc_collection_delete_many(coll, selector, nullptr, &reply, &error)};
    int deleted{0};
    if (ok) {
        deleted = mapFromBson(&reply).value(QStringLiteral("deletedCount")).toInt();
    }
    bson_destroy(&reply);
    bson_destroy(selector);
    mongoc_collection_destroy(coll);
    return deleted;
}

} // namespace SynQt
