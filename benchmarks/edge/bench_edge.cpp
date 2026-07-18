// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A TechEmpower-style HTTP benchmark server on SynQt's own web stack: QHttpServer (the class
// the web edge uses) in front of the QSQLITE engine configured exactly as the sqlite
// persistence provider configures it; WAL, a busy timeout, parameterised queries, and a
// single connection driven from the event loop (SynQt serialises persistence on the entity's
// loop rather than racing threads). The six routes are the canonical TechEmpower test types
// (plaintext, json, single query, multiple queries, updates, fortunes), so the numbers a
// load generator reports here are directly comparable to the framework rows TechEmpower
// publishes. This measures the edge's request stack, not the QtRO live path (that is the
// transport benchmark); together they characterise both halves of the edge.

#include <QCoreApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTcpServer>
#include <QUrlQuery>
#include <QVariant>

namespace {

constexpr int kWorldRows{10000};

// The TechEmpower rule: the queries/updates count is read from the query string, defaulting to
// 1 and clamped to [1, 500]. A missing or non-numeric value means 1.
int clampedQueryCount(const QHttpServerRequest &request)
{
    const QString raw{request.query().queryItemValue(QStringLiteral("queries"))};
    bool ok{false};
    int count{raw.toInt(&ok)};
    if (!ok) {
        return 1;
    }
    return qBound(1, count, 500);
}

int randomWorldId()
{
    return QRandomGenerator::global()->bounded(1, kWorldRows + 1);
}

// HTML-escape a fortune message the way the TechEmpower fortunes test requires (the test seeds
// a row with markup precisely to check the framework escapes it).
QString escapeHtml(const QString &input)
{
    QString out;
    out.reserve(input.size());
    for (const QChar character : input) {
        switch (character.unicode()) {
        case u'&':
            out += QStringLiteral("&amp;");
            break;
        case u'<':
            out += QStringLiteral("&lt;");
            break;
        case u'>':
            out += QStringLiteral("&gt;");
            break;
        case u'"':
            out += QStringLiteral("&quot;");
            break;
        case u'\'':
            out += QStringLiteral("&#39;");
            break;
        default:
            out += character;
            break;
        }
    }
    return out;
}

// Seed the in-memory database with the two TechEmpower tables the way the spec fixes them:
// world holds 10000 rows of a random number, fortune holds twelve messages (one with markup).
bool seedDatabase(QSqlDatabase &database)
{
    QSqlQuery query{database};
    if (!query.exec(QStringLiteral("PRAGMA journal_mode=WAL"))) {
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE world (id INTEGER PRIMARY KEY, randomNumber INTEGER NOT NULL)"))) {
        return false;
    }
    if (!query.exec(QStringLiteral(
            "CREATE TABLE fortune (id INTEGER PRIMARY KEY, message TEXT NOT NULL)"))) {
        return false;
    }
    database.transaction();
    QSqlQuery insertWorld{database};
    insertWorld.prepare(QStringLiteral("INSERT INTO world (id, randomNumber) VALUES (?, ?)"));
    for (int id{1}; id <= kWorldRows; ++id) {
        insertWorld.addBindValue(id);
        insertWorld.addBindValue(QRandomGenerator::global()->bounded(1, kWorldRows + 1));
        if (!insertWorld.exec()) {
            return false;
        }
    }
    const QStringList fortunes{
        QStringLiteral("fortune: No such file or directory"),
        QStringLiteral("A computer scientist is someone who fixes things that aren't broken."),
        QStringLiteral("After enough decimal places, nobody gives a damn."),
        QStringLiteral("A bad random number generator: 1, 1, 1, 1, 1, 4.33e+67, 1, 1, 1"),
        QStringLiteral("A computer program does what you tell it to do, not what you want it to do."),
        QStringLiteral("Emacs is a nice operating system, but I prefer UNIX. (Tom Christiansen)"),
        QStringLiteral("Any program that runs right is obsolete."),
        QStringLiteral("A list is only as strong as its weakest link. (Donald Knuth)"),
        QStringLiteral("Feature: A bug with seniority."),
        QStringLiteral("Computers make very fast, very accurate mistakes."),
        QStringLiteral("<script>alert(\"This should not be displayed in a browser alert box.\");</script>"),
        QStringLiteral("Frameworks come and go; the benchmark abides.")};
    QSqlQuery insertFortune{database};
    insertFortune.prepare(QStringLiteral("INSERT INTO fortune (id, message) VALUES (?, ?)"));
    for (int index{0}; index < fortunes.size(); ++index) {
        insertFortune.addBindValue(index + 1);
        insertFortune.addBindValue(fortunes.at(index));
        if (!insertFortune.exec()) {
            return false;
        }
    }
    return database.commit();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption portOption{QStringLiteral("port"),
        QStringLiteral("Listen port."), QStringLiteral("port"), QStringLiteral("8480")};
    parser.addOption(portOption);
    parser.process(app);

    QSqlDatabase database{QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"))};
    database.setDatabaseName(QStringLiteral(":memory:"));
    database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!database.open()) {
        qCritical().noquote() << "cannot open database:" << database.lastError().text();
        return 1;
    }
    if (!seedDatabase(database)) {
        qCritical().noquote() << "seed failed:" << database.lastError().text();
        return 1;
    }

    QHttpServer server;

    server.route(QStringLiteral("/plaintext"), []() {
        return QHttpServerResponse{QByteArrayLiteral("text/plain"),
                                   QByteArrayLiteral("Hello, World!")};
    });

    server.route(QStringLiteral("/json"), []() {
        return QHttpServerResponse{QJsonObject{{QStringLiteral("message"),
                                                QStringLiteral("Hello, World!")}}};
    });

    server.route(QStringLiteral("/db"), [&database]() {
        QSqlQuery query{database};
        query.prepare(QStringLiteral("SELECT randomNumber FROM world WHERE id = ?"));
        const int id{randomWorldId()};
        query.addBindValue(id);
        query.exec();
        query.next();
        return QHttpServerResponse{QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("randomNumber"), query.value(0).toInt()}}};
    });

    server.route(QStringLiteral("/queries"), [&database](const QHttpServerRequest &request) {
        const int count{clampedQueryCount(request)};
        QSqlQuery query{database};
        query.prepare(QStringLiteral("SELECT randomNumber FROM world WHERE id = ?"));
        QJsonArray rows;
        for (int index{0}; index < count; ++index) {
            const int id{randomWorldId()};
            query.addBindValue(id);
            query.exec();
            query.next();
            rows.append(QJsonObject{{QStringLiteral("id"), id},
                                    {QStringLiteral("randomNumber"), query.value(0).toInt()}});
        }
        return QHttpServerResponse{rows};
    });

    server.route(QStringLiteral("/updates"), [&database](const QHttpServerRequest &request) {
        const int count{clampedQueryCount(request)};
        QSqlQuery select{database};
        select.prepare(QStringLiteral("SELECT randomNumber FROM world WHERE id = ?"));
        QSqlQuery update{database};
        update.prepare(QStringLiteral("UPDATE world SET randomNumber = ? WHERE id = ?"));
        QJsonArray rows;
        database.transaction();
        for (int index{0}; index < count; ++index) {
            const int id{randomWorldId()};
            select.addBindValue(id);
            select.exec();
            select.next();
            const int newNumber{QRandomGenerator::global()->bounded(1, kWorldRows + 1)};
            update.addBindValue(newNumber);
            update.addBindValue(id);
            update.exec();
            rows.append(QJsonObject{{QStringLiteral("id"), id},
                                    {QStringLiteral("randomNumber"), newNumber}});
        }
        database.commit();
        return QHttpServerResponse{rows};
    });

    server.route(QStringLiteral("/fortunes"), [&database]() {
        QSqlQuery query{database};
        query.exec(QStringLiteral("SELECT id, message FROM fortune"));
        QList<QPair<int, QString>> fortunes;
        while (query.next()) {
            fortunes.append({query.value(0).toInt(), query.value(1).toString()});
        }
        fortunes.append({0, QStringLiteral("Additional fortune added at request time.")});
        std::sort(fortunes.begin(), fortunes.end(),
                  [](const QPair<int, QString> &left, const QPair<int, QString> &right) {
                      return left.second < right.second;
                  });
        QString body{QStringLiteral(
            "<!DOCTYPE html><html><head><title>Fortunes</title></head><body>"
            "<table><tr><th>id</th><th>message</th></tr>")};
        for (const auto &fortune : fortunes) {
            body += QStringLiteral("<tr><td>%1</td><td>%2</td></tr>")
                        .arg(fortune.first)
                        .arg(escapeHtml(fortune.second));
        }
        body += QStringLiteral("</table></body></html>");
        return QHttpServerResponse{QByteArrayLiteral("text/html; charset=utf-8"),
                                   body.toUtf8()};
    });

    auto *tcpServer{new QTcpServer{&app}};
    const quint16 port{parser.value(portOption).toUShort()};
    if (!tcpServer->listen(QHostAddress::LocalHost, port) || !server.bind(tcpServer)) {
        qCritical().noquote() << "cannot listen on port" << port;
        return 1;
    }
    qInfo().noquote() << QStringLiteral("bench edge listening on http://127.0.0.1:%1")
                             .arg(tcpServer->serverPort());
    return app.exec();
}
