// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "mongoprovider.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <mongoc/mongoc.h>

#include <utility>

namespace SynQt {

namespace {

// Bridge a map to a BSON document via its JSON form (the driver ships a JSON<->BSON codec,
// so this stays an honest wrapper rather than a hand-rolled BSON builder).
bson_t *bsonFromMap(const QVariantMap &map)
{
    const QByteArray json{
        QJsonDocument{QJsonObject::fromVariantMap(map)}.toJson(QJsonDocument::Compact)};
    bson_error_t error;
    bson_t *document{bson_new_from_json(reinterpret_cast<const uint8_t *>(json.constData()),
                                        json.size(), &error)};
    return document != nullptr ? document : bson_new();
}

QVariantMap mapFromBson(const bson_t *document)
{
    char *json{bson_as_relaxed_extended_json(document, nullptr)};
    if (json == nullptr) {
        return QVariantMap{};
    }
    const QVariantMap map{QJsonDocument::fromJson(QByteArray{json}).object().toVariantMap()};
    bson_free(json);
    return map;
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
    // A document store off-host must ride TLS in release; only dev may relax it.
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
    mongoc_collection_t *coll{mongoc_client_get_collection(
        static_cast<mongoc_client_t *>(m_client), m_config.database.toUtf8().constData(),
        collection.toUtf8().constData())};
    bson_t *doc{bsonFromMap(document)};

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
    mongoc_collection_t *coll{mongoc_client_get_collection(
        static_cast<mongoc_client_t *>(m_client), m_config.database.toUtf8().constData(),
        collection.toUtf8().constData())};
    bson_t *query{bsonFromMap(filter)};
    bson_t *opts{bsonFromMap(options)};
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
    mongoc_collection_t *coll{mongoc_client_get_collection(
        static_cast<mongoc_client_t *>(m_client), m_config.database.toUtf8().constData(),
        collection.toUtf8().constData())};
    bson_t *selector{bsonFromMap(filter)};
    bson_t *update{bsonFromMap(QVariantMap{{QStringLiteral("$set"), change}})};

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
    mongoc_collection_t *coll{mongoc_client_get_collection(
        static_cast<mongoc_client_t *>(m_client), m_config.database.toUtf8().constData(),
        collection.toUtf8().constData())};
    bson_t *selector{bsonFromMap(filter)};

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
