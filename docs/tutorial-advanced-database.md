<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# A database of your own

Goal: put a relational entity in front of Microsoft SQL Server, which has no bundled
provider, without changing a line of the entity's QML or of any consumer. At the end,
`provider.name: custom:SqlServer` is the only difference from the SQLite it started on.

SQL Server makes a good first adaptor because Qt does most of the work. It has no native Qt
driver, but Qt reaches it through
[QODBC](https://doc.qt.io/qt-6/sql-driver.html#qodbc-for-open-database-connectivity-odbc),
so statements, binding and results use the Qt SQL API you may know. You handle the
connection, the SQL dialect and the security rules.
[The next page](tutorial-advanced-cache.md) covers the harder case: an engine Qt cannot
reach at all.

## Step 1: Know what you are signing up for

The persistence family is `IPersistenceProvider`, with ten functions: three for
lifecycle, two for statements, three for transactions, one for migrations, and one that
names the provider:

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

Two rules shape the interface.

**The SQL and its parameters arrive separately.** Every overload keeps them apart, so a
provider never receives a statement with a value already pasted in, and your adaptor has
no place where an injection could slip in. Whatever your engine's binding syntax, bind;
never concatenate.

**Errors come back in the return value.** `DbResult` carries `ok`, `error` and the data,
and the functions with three outcomes take a `QString *error`. Errors never cross this
boundary as exceptions: the caller is an entity's event loop, and an exception unwinding through it
would take the entity down over a failed `SELECT`.

## Step 2: Scaffold it

```cli
synqt add provider SqlServer --family relational
```

This writes `providers/custom/sqlserverprovider.cpp`, with the class, the registration,
and every operation stubbed to report that it is not implemented. It compiles and
registers as is, so you can select it at once and see it fail with a clear message. The
rest of this page fills it in.

The file is compiled into every entity whose config selects `custom:SqlServer`; that
selection is the only wiring. Every build writes `generated/synqt.cmake` from your
topology, and your root `CMakeLists.txt` only includes it, so there is no CMake to edit.

## Step 3: Open the connection, or refuse to

Everything about reaching the engine lives here and nowhere else: the driver, the
address, the credentials, and the one question SynQt makes every provider answer: is the
connection verified?

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
    // The verification switch, inverted. TrustServerCertificate=yes is the engine saying
    // "do not check who I am", which is exactly what an unverified mode means.
    attributes.append(QStringLiteral("TrustServerCertificate=%1")
                          .arg(isVerified(config.sslMode) ? QStringLiteral("no")
                                                          : QStringLiteral("yes")));
    if (!config.caCert.isEmpty()) {
        // ODBC Driver 18.1 and later. With no ca_cert the driver uses the machine's own
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
                // Through the API rather than the string. QODBC escapes what it is handed
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

This code follows three mandatory rules:

- **The credentials never leave.** `m_config.password` comes from the entity's own
  environment, through an `env:` reference the build refuses to resolve in a client
  target. It goes into the connection and nowhere else: never a log line, an error
  message or a connect point property. The error above names the host, not the
  connection string.
- **An unverified connection is refused in release.** `refusesInsecure()`, two steps
  below, is that whole policy. Development on loopback stays easy, and a release build
  pointed at a real address with verification off does not start. Every adaptor inherits
  this rule. See
  [security of third party backends](providers.md#security-of-third-party-backends).
- **The connection belongs to one thread.** Qt SQL requires that a `QSqlDatabase` be used
  only on the thread that created it, and `SqlConnectionPool` is built for that. That
  thread is the entity's event loop. Never hand a lease to a worker.

## Step 4: Run a statement

Every request from the entity arrives through two functions, which are the same function
with a flag. From here on the class appears in the order of the explanation, so
`public:` and `private:` alternate more than in a normal file. Joined together the pieces
are valid C++, and reordering them changes nothing.

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

The shared part is short because `runStatement()`, from the framework's SQL support,
already prepares, binds and collects rows for any Qt SQL driver. Reuse it: it enforces the
"bind, never concatenate" rule, and a provider that writes its own version risks a
hole.

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

The branch on `m_inTransaction` is easy to forget in a hand-written relational provider.
A pool hands out whichever connection is free, but a transaction lives on one connection.
Take a fresh lease inside a transaction, and the statement commits on its own while the
transaction it belonged to rolls back. The symptom is half-written data that no test
reproduces.

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

The two endings share their bookkeeping, since they differ only in the function they
call:

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

A migration list is the schema's history, and `migrate()` receives all of it every time
the entity starts. It applies the steps not yet applied, in order, and does nothing when
there are none. It never goes backwards, and has no `down`, because a rollback run
against production data destroys data while looking like a safety measure.

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
steps in one transaction, and records the new version. The table is portable ANSI SQL, so
the only dialect-specific line in this adaptor is the `IF OBJECT_ID(...)` in `connect()`
that created it.

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
// linking the file into the entity is all the wiring there is. The name here is bare,
// with no `custom:` prefix, because the prefix is what routes a lookup to this registry.
SYNQT_REGISTER_PERSISTENCE_PROVIDER("SqlServer", SqlServerProvider)

} // namespace SynQt
```

The pieces from step 3 onward, in order, make the whole file.

## Step 8: Select it

Add one block to `synqt.yaml`; nothing else in the project changes:

```yaml
entities:
  - name: books
    type: relational
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

and put the secret's name, never its value, in `.env.example`:

```text
MSSQL_PASSWORD=
```

The entity's connect point Source (`db/relational/books/Books.qml`, or whatever you named
it) is unchanged: it called `Db.query(...)` before and still does. The engine changed and
the contract did not, so no consumer notices anything.

## Try it, then think

> [!QUESTION]
> Set `provider.name: custom:SqlSever` (note the typo) and start the entity. Then fix the
> typo, set `sslmode: prefer` in a release build, and start it again. What happens each
> time, and why is it not a warning?

<details class="solution" markdown>
<summary>Solution</summary>

With the typo, the family factory sends any `custom:` name to the registry, finds nothing
under `SqlSever`, and the entity refuses to start, listing the providers the persistence
family does have. Otherwise the entity would come up with a connect point whose every call
fails at run time, a worse problem that a user would find later.

With `sslmode: prefer`, it refuses to start too, and the message says why: `connect()`
returns false from `refusesInsecure()` before opening a socket. A warning would be wrong,
because you can ship past a warning. The refused connection would hand the engine's
credentials, and every row after them, to whoever answered at that address.

Both follow one principle: a misconfiguration that would produce a system that looks fine
but has a hole fails at startup instead.

</details>

## What you learned

- A provider is the only part of an entity that knows the engine. Implementing a family
  interface is all it takes to reach a new engine.
- SQL and parameters always arrive separately. Bind them; a correct adaptor never
  concatenates a value into a statement.
- Errors are returned, never thrown, because the caller is an entity's event loop.
- A pooled provider must pin a transaction to one connection, or the transaction silently
  misses statements it appears to contain.
- Credentials come from the entity environment, go into the connection, and appear
  nowhere else.
- The adaptor refuses an unverified connection to a real address in a release build, at
  startup.
- `custom:` is a namespace. A lookup with it reaches only your registrations, so nothing
  you register can shadow `sqlite`, and a name that selects nothing stops the entity.

If you write one of these for a real engine, please
[send it](tutorial-advanced.md#when-yours-works-send-it), so nobody else has to write
it.
