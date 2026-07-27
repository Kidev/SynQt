<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# A database of your own

Goal: put a persistence entity in front of Microsoft SQL Server, which SynQt does not
bundle a provider for, without changing one line of the entity's QML or of any consumer.
By the end, `provider.name: custom:SqlServer` will be all that separates the entity from
the SQLite it started on.

SQL Server is a good first adaptor because Qt does most of the work. It has no native Qt
driver, but it is reachable through
[QODBC](https://doc.qt.io/qt-6/sql-driver.html#qodbc-for-open-database-connectivity-odbc),
so the statements, the binding, and the result handling are the Qt SQL API you may
already know. What is yours is the connection, the dialect, and the security position.
[The next page](tutorial-advanced-cache.md) does the harder shape, an engine Qt cannot
reach at all.

## Step 1: Know what you are signing up for

The persistence family is `IPersistenceProvider`, and it is ten functions. Three are
lifecycle, two are statements, three are transactions, one is migrations, and one names
the provider:

```cpp
bool connect(QString *error);
void disconnect();
bool isHealthy() const;

DbResult query(const QString &sql, const QVariantList &params);   // SELECT
DbResult exec(const QString &sql, const QVariantList &params);    // INSERT/UPDATE/DDL

bool begin(QString *error);
bool commit(QString *error);
bool rollback(QString *error);

bool migrate(const QStringList &steps, QString *error);

QString name() const;
```

Two things in that list are load bearing, and they are the reason the interface looks the
way it does.

The SQL and its parameters arrive **separately**, and there is no overload that takes
them together. A provider is never handed a finished statement with a value already
pasted into it, so there is no place in your adaptor where an injection could be
introduced even by accident. Whatever your engine's binding syntax is, it is your job to
bind, never to concatenate.

Errors come back in the return value. `DbResult` carries `ok`, `error`, and the data;
the three-state functions take a `QString *error`. Nothing is thrown across this
boundary, because the thing on the other side of it is an entity's event loop, and an
exception unwinding through that would take the entity down over a failed `SELECT`.

## Step 2: Scaffold it

```cli
synqt add provider SqlServer --family persistence
```

That writes `providers/custom/sqlserverprovider.cpp`: the class, the registration, and
every operation stubbed to report that it is not written yet. It compiles and registers
as it stands, so you can select it immediately and watch it fail honestly rather than
quietly. The rest of this page fills it in.

The file is compiled into any entity whose config selects `custom:SqlServer`, and that
selection is the only wiring there is. There is no CMake to edit, and you should not try:
the root `CMakeLists.txt` is regenerated from your topology on every build.

## Step 3: Open the connection, or refuse to

This is the part that is genuinely yours. Everything about how the engine is reached
lives here and nowhere else: the driver, the address, the credentials, and the answer to
the one question SynQt does not let a provider dodge, which is whether the connection is
verified.

```cpp
#include "ipersistenceprovider.h"
#include "providerconfig.h"
#include "providerregistry.h"
#include "sqlconnectionpool.h"
#include "sqlsupport.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <memory>
#include <utility>

namespace SynQt {

namespace {

// "Encrypted" and "verified" are different claims, and only the second one counts. A
// connection that encrypts to whoever answered the address protects the traffic from a
// passive listener and not at all from the machine that intercepted it.
bool isVerified(const QString &sslMode)
{
    return sslMode == QLatin1String("verify-ca") || sslMode == QLatin1String("verify-full");
}

// A DSN-less ODBC connection string. QODBC takes the whole thing through
// setDatabaseName(), so an adaptor for any other ODBC-reachable engine is this function
// with a different Driver= and the same everything else.
QString connectionString(const ProviderConfig &config)
{
    const int port{config.port > 0 ? config.port : 1433};
    QStringList attributes;
    attributes.append(QStringLiteral("Driver={ODBC Driver 18 for SQL Server}"));
    attributes.append(QStringLiteral("Server=tcp:%1,%2").arg(config.host,
                                                             QString::number(port)));
    attributes.append(QStringLiteral("Database=%1").arg(config.database));
    attributes.append(QStringLiteral("Encrypt=yes"));
    // The verification switch, inverted: TrustServerCertificate=yes is the engine saying
    // "do not check who I am", which is exactly what an unverified mode means.
    attributes.append(QStringLiteral("TrustServerCertificate=%1")
                          .arg(isVerified(config.sslMode) ? QStringLiteral("no")
                                                          : QStringLiteral("yes")));
    if (!config.caCert.isEmpty()) {
        // ODBC Driver 18.1 and later; with no ca_cert the driver uses the machine's own
        // trust store, which is the right answer for a managed engine with a public CA.
        attributes.append(QStringLiteral("ServerCertificate=%1").arg(config.caCert));
    }
    return attributes.join(QLatin1Char(';'));
}

} // namespace

class SqlServerProvider final : public IPersistenceProvider
{
public:
    explicit SqlServerProvider(ProviderConfig config)
        : m_config{std::move(config)}
    {
    }

    ~SqlServerProvider() override
    {
        disconnect();
    }

    QString name() const override { return QStringLiteral("custom:SqlServer"); }

    bool connect(QString *error) override
    {
        if (refusesInsecure()) {
            if (error != nullptr) {
                *error = QStringLiteral(
                    "refusing an unverified connection to %1 in release: set sslmode to "
                    "verify-full").arg(m_config.host);
            }
            return false;
        }

        const ProviderConfig config{m_config};
        m_pool = std::make_unique<SqlConnectionPool>(
            QStringLiteral("QODBC"),
            [config](QSqlDatabase &db) {
                db.setDatabaseName(connectionString(config));
                // Through the API rather than the string: QODBC escapes what it is handed
                // here, and a password containing a `;` would otherwise end the attribute
                // it sits in and turn the rest of the string into something else.
                db.setUserName(config.user);
                db.setPassword(config.password);
                db.setConnectOptions(QStringLiteral("SQL_ATTR_ODBC_VERSION=SQL_OV_ODBC3"));
            },
            m_config.poolSize);

        // Open one connection now, so a wrong address or a bad credential is a startup
        // failure with a message rather than a mystery on the first browser request. The
        // pool keeps it for reuse.
        SqlConnectionPool::Lease lease{m_pool->acquire(error)};
        if (!lease.isValid()) {
            m_pool.reset();
            return false;
        }
        return runStatement(lease.database(),
                            QStringLiteral("IF OBJECT_ID('synqt_migrations') IS NULL "
                                           "CREATE TABLE synqt_migrations (version INT NOT NULL)"),
                            {}, false)
            .ok;
    }

    void disconnect() override
    {
        m_transaction = SqlConnectionPool::Lease{};
        m_inTransaction = false;
        if (m_pool) {
            m_pool->closeAll();
            m_pool.reset();
        }
    }

    // Real readiness, so the entity can report not ready and retry. An adaptor that
    // always answers true turns a dead engine into a connect point whose every call
    // fails for no stated reason.
    bool isHealthy() const override
    {
        return m_pool != nullptr && m_pool->openCount() > 0;
    }
```

Three rules are being obeyed here, and none of them is optional.

**The credentials never leave.** `m_config.password` came from the entity's own
environment, through an `env:` reference the build refuses to resolve in a client target.
It is written into the connection and into nothing else: not a log line, not an error
message, not a property on a connect point. The error above names the host, deliberately,
and not the string the host was reached with.

**An unverified connection is refused in release.** `refusesInsecure()`, two steps below,
is the whole of that policy. Development on loopback stays easy, and a release build
pointed at a real address with verification off does not start. This is a rule an adaptor
inherits, not one it decides: see
[security of third party backends](providers.md#security-of-third-party-backends).

**The connection belongs to one thread.** Qt SQL requires that a `QSqlDatabase` be used
only on the thread that created it, and `SqlConnectionPool` is built around that. The
entity's event loop is that thread. Do not hand a lease to a worker.

## Step 4: Run a statement

Everything the entity actually asks for comes through two functions, and both are the
same function with a flag. From here the class is shown in the order the story goes, so
`public:` and `private:` alternate more than they would in a file you sat down and wrote;
the concatenation is valid C++, and sorting it afterwards changes nothing.

```cpp
    DbResult query(const QString &sql, const QVariantList &params) override
    {
        return run(sql, params, true);
    }

    DbResult exec(const QString &sql, const QVariantList &params) override
    {
        return run(sql, params, false);
    }
```

The shared half is short because `runStatement()`, from the framework's SQL support,
already does the prepare, the bind, and the row collection for any Qt SQL driver. Reuse
it: it is where the "bind, never concatenate" rule is actually enforced, and writing your
own version is how a provider grows a hole.

```cpp
private:
    DbResult run(const QString &sql, const QVariantList &params, bool collectRows)
    {
        if (!m_pool) {
            return DbResult::failure(QStringLiteral("provider not connected"));
        }
        // Inside a transaction every statement rides the one pinned connection, or it
        // would not be in the transaction at all. Outside one, each caller takes its own
        // lease, which is what lets two requests overlap.
        if (m_inTransaction && m_transaction.isValid()) {
            return runStatement(m_transaction.database(), sql, params, collectRows);
        }
        QString error;
        SqlConnectionPool::Lease lease{m_pool->acquire(&error)};
        if (!lease.isValid()) {
            return DbResult::failure(error);
        }
        return runStatement(lease.database(), sql, params, collectRows);
    }
```

That branch on `m_inTransaction` is the single most common bug in a hand-written
persistence provider. A pool hands out whichever connection is free, and a transaction
lives on one connection; take a fresh lease inside a transaction and the statement is
committed independently while the transaction it was supposed to be part of rolls back
around it. The symptom is half-written data that no test reproduces.

## Step 5: Transactions

```cpp
public:
    bool begin(QString *error) override
    {
        if (!m_pool) {
            return fail(error, "provider not connected");
        }
        if (m_inTransaction) {
            return fail(error, "a transaction is already open");
        }
        m_transaction = m_pool->acquire(error);
        if (!m_transaction.isValid()) {
            return false;
        }
        if (!m_transaction.database().transaction()) {
            if (error != nullptr) {
                *error = m_transaction.database().lastError().text();
            }
            m_transaction = SqlConnectionPool::Lease{};
            return false;
        }
        m_inTransaction = true;
        return true;
    }

    bool commit(QString *error) override { return finish(error, true); }

    bool rollback(QString *error) override { return finish(error, false); }
```

with the two endings sharing their bookkeeping, since the only difference between them is
which function they call and everything after that is identical:

```cpp
private:
    bool finish(QString *error, bool commitIt)
    {
        if (!m_inTransaction) {
            return fail(error, "no transaction is open");
        }
        const bool ok{commitIt ? m_transaction.database().commit()
                               : m_transaction.database().rollback()};
        if (!ok && error != nullptr) {
            *error = m_transaction.database().lastError().text();
        }
        // Released either way. A transaction that failed to commit is still over, and a
        // lease held past it is a connection the pool has lost.
        m_transaction = SqlConnectionPool::Lease{};
        m_inTransaction = false;
        return ok;
    }

    bool fail(QString *error, const char *message) const
    {
        if (error != nullptr) {
            *error = QString::fromLatin1(message);
        }
        return false;
    }
```

## Step 6: Migrations, forward only

A migration list is the schema's history, and `migrate()` is handed all of it every time
the entity starts. Its job is to apply the steps that have not been applied yet, in order,
and to be a no-op when there are none. It never goes backwards: there is no `down`, on
purpose, because a rollback that runs against production data is a data loss feature
wearing a safety label.

```cpp
public:
    bool migrate(const QStringList &steps, QString *error) override
    {
        if (!m_pool) {
            return fail(error, "provider not connected");
        }
        SqlConnectionPool::Lease lease{m_pool->acquire(error)};
        if (!lease.isValid()) {
            return false;
        }
        return applyMigrations(lease.database(), steps, error);
    }
```

`applyMigrations()` reads the applied count from `synqt_migrations`, runs the remaining
steps inside one transaction, and records the new version. The table is portable ANSI
SQL, which is why the only dialect-specific line in this whole adaptor is the
`IF OBJECT_ID(...)` that created it back in `connect()`.

## Step 7: Close the file

The last two members, and the registration that makes the name selectable:

```cpp
private:
    bool refusesInsecure() const
    {
        return m_config.release && !m_config.isLoopbackHost() && !isVerified(m_config.sslMode);
    }

    ProviderConfig m_config;
    std::unique_ptr<SqlConnectionPool> m_pool;
    SqlConnectionPool::Lease m_transaction;
    bool m_inTransaction{false};
};

// This line is what makes the class reachable. It runs at static initialization, so
// linking the file into the entity is the whole of the wiring. The name here is bare:
// no `custom:` prefix, because the prefix is what routes a lookup to this registry.
SYNQT_REGISTER_PERSISTENCE_PROVIDER("SqlServer", SqlServerProvider)

} // namespace SynQt
```

Read the chunks from Step 3 onward in order and you have the file.

## Step 8: Select it

One block in `synqt.yaml`, and nothing else in the project changes:

```yaml
entities:
  - name: database
    kind: service
    blueprint: persistence
    provider:
      name: custom:SqlServer
      host: sql.internal              # a private address, never public
      port: 1433
      database: app
      user: app
      password: env:MSSQL_PASSWORD    # entity .env only, never logged
      sslmode: verify-full
      pool_size: 8
```

and the secret's name, never its value, in `.env.example`:

```text
MSSQL_PASSWORD=
```

The entity's connect point Source, `database/Items.qml` or whatever you called it, is
untouched. It called `Db.query(...)` before and it calls `Db.query(...)` now. That is the
masking working: the engine changed and the contract did not, so no consumer had anything
to notice.

## Try it, then think

> [!QUESTION]
> Set `provider.name: custom:SqlSever` (note the typo) and start the entity. Then put the
> `s` back but set `sslmode: prefer` in a release build and start it again. What happens
> each time, and why is it that rather than a warning?

<details class="solution" markdown>
<summary>Solution</summary>

The typo does not start. The family factory sends any `custom:` name to the registry,
finds nothing registered under `SqlSever`, and the entity refuses to start, naming the
providers the persistence family does have. The alternative would be an entity that comes
up with a connect point whose every call fails at run time, which is a worse outcome
discovered later, by a user.

`sslmode: prefer` does not start either, and the message says so: `connect()` returns
false from `refusesInsecure()` before a socket is opened. A warning would be the wrong
shape, because a warning is a thing you can ship past. The connection this refuses to
open is one that would hand the engine's credentials, and every row that follows, to
whoever answered that address.

Both are the same principle: a misconfiguration that would produce a working-looking
system with a hole in it fails at startup instead.

</details>

## What you learned

- A provider is the one part of an entity that knows the engine, and implementing a
  family interface is the whole of what makes an engine reachable.
- SQL and parameters arrive separately, always. Bind them; there is no code path in a
  correct adaptor that concatenates a value into a statement.
- Errors are returned, never thrown, because the caller is an entity's event loop.
- A pooled provider must pin its transaction to one connection, or a transaction will
  silently not contain the statements it appears to.
- Credentials come from the entity environment, go into the connection, and appear
  nowhere else.
- An unverified connection to a real address is refused in a release build, by the
  adaptor, at startup.
- `custom:` is a namespace: a lookup carrying it reaches your registrations only, so
  nothing you register can shadow `sqlite`, and a name that selects nothing stops the
  entity rather than degrading it.

If you write one of these for a real engine, please
[send it](tutorial-advanced.md#when-yours-works-send-it): a provider that works is one
somebody else does not have to write.
