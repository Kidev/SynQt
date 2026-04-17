<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# A cache of your own

On [the database page](tutorial-advanced-database.md), Qt did most of the work: a driver
existed, so the adaptor was a connection and a dialect. This page covers the other case.
Memcached has no Qt driver, no Qt module and no client library in SynQt, so the adaptor
implements the protocol by hand over a socket.

Most engines worth adapting are like this, and the protocol is usually the easy part. The
hard part is handling the places where the engine and the interface disagree. Memcached
disagrees in three.

## Step 1: A smaller interface, and a different error model

The cache family is `ICacheProvider`, with nine functions:

```cpp
bool connect(QString *error);
void disconnect();
bool isHealthy() const;

QVariant get(const QString &key);                                  // invalid if missing
void set(const QString &key, const QVariant &value, int ttlSeconds);
void del(const QString &key);
qint64 incr(const QString &key, qint64 by);                        // returns the new value
void expire(const QString &key, int ttlSeconds);

QString name() const;
```

`set`, `del` and `expire` return nothing, and `get` cannot report a failure: a missing
value and an unreachable engine both return an invalid `QVariant`. That is the family's
contract. A cache is an optimization, so a miss is a normal result, and a broken cache
makes a system slow, not broken. Callers may ignore the difference, so your adaptor must
never turn a cache problem into an application problem: no throwing, no blocking forever,
and no returning data it is not sure is fresh.

`incr` is the one that returns something, and it is where this engine gets tricky.

## Step 2: Scaffold and connect

```cli
synqt add provider Memcached --family cache
```

Then the connection. Memcached listens on port 11211 and speaks a line based text
protocol. It supports TLS only in builds configured for it, but the rule still holds: a
release build refuses an unverified connection to a real address, here as everywhere.

```cpp
#include "icacheprovider.h"
#include "providerconfig.h"
#include "providerregistry.h"

#include <QByteArray>
#include <QDataStream>
#include <QList>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <memory>
#include <utility>

namespace SynQt {

namespace {

// How long any single exchange may take. A cache that blocks an entity's event loop is
// worse than no cache: every consumer of every connect point on that entity waits behind
// it. Short, and a timeout is a miss.
constexpr int ExchangeTimeoutMs{250};

// The `flags` field memcached stores alongside each value and hands back on a get. It is
// opaque to the engine and meant for exactly this: recording how the client encoded the
// bytes.
constexpr quint32 Opaque{0};   ///< a QDataStream-serialized QVariant
constexpr quint32 Counter{1};  ///< decimal text, so the engine's own incr can read it

bool isCounter(const QVariant &value)
{
    const int id{value.typeId()};
    return id == QMetaType::Int || id == QMetaType::UInt || id == QMetaType::LongLong
           || id == QMetaType::ULongLong;
}

QByteArray encode(const QVariant &value, quint32 *flags)
{
    // An integer is stored as its decimal text and nothing else. Memcached's incr parses
    // the stored bytes itself, so the moment you want the engine's atomic counter, the
    // engine dictates how numbers are written.
    if (isCounter(value)) {
        *flags = Counter;
        return QByteArray::number(value.toLongLong());
    }
    *flags = Opaque;
    QByteArray payload;
    QDataStream stream{&payload, QIODevice::WriteOnly};
    // Pinned, so an entry written before a Qt upgrade is still readable after one.
    stream.setVersion(QDataStream::Qt_6_0);
    stream << value;
    return payload;
}

QVariant decode(const QByteArray &payload, quint32 flags)
{
    if (flags == Counter) {
        return QVariant{payload.toLongLong()};
    }
    QDataStream stream{payload};
    stream.setVersion(QDataStream::Qt_6_0);
    QVariant value;
    stream >> value;
    return value;
}

// A key on the wire. memcached keys may not contain a space or a control character, and
// the protocol is line-based, so a key carrying either would stop being a key and become
// the rest of the command. Percent-encoding is the whole defence, and it is this family's
// version of binding a SQL parameter. The caller's bytes never reach the engine as syntax.
QByteArray wireKey(const QString &key)
{
    const QByteArray encoded{QUrl::toPercentEncoding(key)};
    return encoded.size() <= 250 ? encoded : QByteArray{};
}

} // namespace
```

The socket, and the lifecycle:

```cpp
class MemcachedProvider final : public ICacheProvider
{
public:
    explicit MemcachedProvider(ProviderConfig config)
        : m_config{std::move(config)}
    {
    }

    ~MemcachedProvider() override
    {
        disconnect();
    }

    QString name() const override { return QStringLiteral("custom:Memcached"); }

    bool connect(QString *error) override
    {
        if (m_config.release && !m_config.isLoopbackHost() && !m_config.tls) {
            if (error != nullptr) {
                *error = QStringLiteral("refusing a plaintext cache connection to %1 in "
                                        "release: set tls: true").arg(m_config.host);
            }
            return false;
        }

        const int port{m_config.port > 0 ? m_config.port : 11211};
        m_socket = std::make_unique<QSslSocket>();
        if (m_config.tls) {
            QSslConfiguration ssl{QSslConfiguration::defaultConfiguration()};
            ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
            if (!m_config.caCert.isEmpty()) {
                ssl.setCaCertificates(QSslCertificate::fromPath(m_config.caCert));
            }
            m_socket->setSslConfiguration(ssl);
            m_socket->connectToHostEncrypted(m_config.host, static_cast<quint16>(port));
            if (!m_socket->waitForEncrypted(ExchangeTimeoutMs * 4)) {
                if (error != nullptr) {
                    *error = m_socket->errorString();
                }
                m_socket.reset();
                return false;
            }
        } else {
            // Dev on loopback only. The check above already refused anything else.
            m_socket->connectToHost(m_config.host, static_cast<quint16>(port));
            if (!m_socket->waitForConnected(ExchangeTimeoutMs * 4)) {
                if (error != nullptr) {
                    *error = m_socket->errorString();
                }
                m_socket.reset();
                return false;
            }
        }

        // One round trip, so a wrong port answers here rather than on the first get.
        if (!writeAll(QByteArrayLiteral("version\r\n")) || !readLine().startsWith("VERSION")) {
            if (error != nullptr) {
                *error = QStringLiteral("%1:%2 did not answer as memcached")
                             .arg(m_config.host, QString::number(port));
            }
            m_socket.reset();
            return false;
        }
        return true;
    }

    void disconnect() override
    {
        if (m_socket) {
            m_socket->disconnectFromHost();
            m_socket.reset();
        }
    }

    bool isHealthy() const override
    {
        return m_socket != nullptr && m_socket->state() == QAbstractSocket::ConnectedState;
    }
```

The socket is synchronous: each exchange blocks the entity's event loop for at most a
quarter second. That suits a cache lookup that normally takes under a millisecond on a
private network, which is why the timeout is short and counts as a miss instead of being
retried. An engine slower than that belongs in an entity of its own with a connect point,
not behind a synchronous family interface.

## Step 3: The protocol

Two helpers carry every command, and they are all the wire handling:

```cpp
private:
    bool writeAll(const QByteArray &bytes)
    {
        if (!isHealthy()) {
            return false;
        }
        return m_socket->write(bytes) == bytes.size()
               && m_socket->waitForBytesWritten(ExchangeTimeoutMs);
    }

    // One protocol line, without its terminator. An empty result means the exchange timed
    // out, which every caller below treats as a miss.
    QByteArray readLine()
    {
        while (isHealthy() && !m_socket->canReadLine()) {
            if (!m_socket->waitForReadyRead(ExchangeTimeoutMs)) {
                return QByteArray{};
            }
        }
        QByteArray line{m_socket->readLine()};
        while (line.endsWith('\n') || line.endsWith('\r')) {
            line.chop(1);
        }
        return line;
    }

    // A value body, whose length the VALUE header just told us. Read by count, never by
    // line, because the payload is arbitrary bytes and may contain a newline of its own.
    QByteArray readBody(qsizetype count)
    {
        QByteArray body;
        while (body.size() < count) {
            if (m_socket->bytesAvailable() == 0
                && !m_socket->waitForReadyRead(ExchangeTimeoutMs)) {
                return QByteArray{};
            }
            body.append(m_socket->read(count - body.size()));
        }
        readLine();  // the CRLF that closes the body
        return body;
    }
```

`get` and `set` then follow the protocol almost line for line:

```cpp
public:
    QVariant get(const QString &key) override
    {
        const QByteArray wire{wireKey(key)};
        if (wire.isEmpty() || !writeAll("get " + wire + "\r\n")) {
            return QVariant{};
        }
        // "VALUE <key> <flags> <bytes>" then the body, or "END" on a miss.
        const QByteArray header{readLine()};
        if (!header.startsWith("VALUE ")) {
            return QVariant{};
        }
        const QList<QByteArray> parts{header.split(' ')};
        if (parts.size() < 4) {
            return QVariant{};
        }
        const QByteArray body{readBody(parts.at(3).toLongLong())};
        readLine();  // the trailing END
        return decode(body, parts.at(2).toUInt());
    }

    void set(const QString &key, const QVariant &value, int ttlSeconds) override
    {
        const QByteArray wire{wireKey(key)};
        if (wire.isEmpty()) {
            return;
        }
        quint32 flags{Opaque};
        const QByteArray payload{encode(value, &flags)};
        // exptime 0 means no expiry, and anything over 30 days is read as an absolute
        // Unix time rather than a duration. Clamping keeps a caller's "one year" from
        // becoming "a moment in 1970".
        const int expiry{ttlSeconds > 0 ? qMin(ttlSeconds, 2592000) : 0};
        const QByteArray command{"set " + wire + " " + QByteArray::number(flags) + " "
                                 + QByteArray::number(expiry) + " "
                                 + QByteArray::number(payload.size()) + "\r\n"};
        if (writeAll(command + payload + "\r\n")) {
            readLine();  // STORED, and a cache write that failed is a cache miss later
        }
    }

    void del(const QString &key) override
    {
        const QByteArray wire{wireKey(key)};
        if (!wire.isEmpty() && writeAll("delete " + wire + "\r\n")) {
            readLine();  // DELETED or NOT_FOUND, and neither is the caller's problem
        }
    }

    void expire(const QString &key, int ttlSeconds) override
    {
        const QByteArray wire{wireKey(key)};
        const int expiry{ttlSeconds > 0 ? qMin(ttlSeconds, 2592000) : 0};
        if (!wire.isEmpty()
            && writeAll("touch " + wire + " " + QByteArray::number(expiry) + "\r\n")) {
            readLine();  // TOUCHED or NOT_FOUND
        }
    }
```

## Step 4: The three disagreements

Memcached and `ICacheProvider` disagree about counters in three ways.

- **Memcached leaves a missing counter missing.** `incr` on a missing key returns `NOT_FOUND`
  instead of starting at zero. The interface promises the new value, so you may not pass
  "the key was missing" upward. Use `add`, which stores only if the key is still absent,
  so a race with another entity doing the same thing resolves cleanly: whoever loses the
  `add` increments what the winner created.
- **`incr` only counts up.** Memcached has `incr` and a separate `decr`, while the
  interface's `by` is signed. Choose the command from the sign.
- **It stops at zero.** `decr` past zero gives zero, not a negative number, and nothing
  outside the engine can change that. So say so in the code, where someone expecting a
  counter to go negative will read it. Document the limitation instead of hiding it behind
  a read-modify-write that is no longer atomic.

```cpp
    qint64 incr(const QString &key, qint64 by) override
    {
        const QByteArray wire{wireKey(key)};
        if (wire.isEmpty()) {
            return 0;
        }
        const qint64 stepped{step(wire, by)};
        if (stepped >= 0) {
            return stepped;
        }

        // Missing: create it at `by`, atomically. `add` stores only if the key is still
        // absent, so if another caller created it in the meantime we lose the add and
        // step theirs instead, which is the same result either way round.
        const QByteArray seed{QByteArray::number(by > 0 ? by : 0)};
        const QByteArray command{"add " + wire + " " + QByteArray::number(Counter) + " 0 "
                                 + QByteArray::number(seed.size()) + "\r\n"};
        if (!writeAll(command + seed + "\r\n")) {
            return 0;
        }
        if (readLine() == QByteArrayLiteral("STORED")) {
            return seed.toLongLong();
        }
        // Lost the race. One retry rather than a loop, because if the key is missing again
        // the engine is not behaving, and a cache is allowed to give up rather than spin.
        const qint64 retried{step(wire, by)};
        return retried >= 0 ? retried : 0;
    }

private:
    /// One incr/decr round trip. -1 means the key was missing or the exchange failed.
    /// Memcached counters are unsigned, so a real answer is never negative.
    qint64 step(const QByteArray &wire, qint64 by)
    {
        // decr floors at zero. This engine has no negative counters, and emulating them
        // with a get/set pair would trade away the one property a counter is used for.
        const QByteArray command{by < 0 ? QByteArrayLiteral("decr") : QByteArrayLiteral("incr")};
        const QByteArray amount{QByteArray::number(by < 0 ? -by : by)};
        if (!writeAll(command + " " + wire + " " + amount + "\r\n")) {
            return -1;
        }
        const QByteArray answer{readLine()};
        if (answer.isEmpty() || answer == QByteArrayLiteral("NOT_FOUND")) {
            return -1;
        }
        return answer.toLongLong();
    }
```

Then the members and the registration:

```cpp
private:
    ProviderConfig m_config;
    std::unique_ptr<QSslSocket> m_socket;
};

SYNQT_REGISTER_CACHE_PROVIDER("Memcached", MemcachedProvider)

} // namespace SynQt
```

## Step 5: Select it

```yaml
entities:
  - name: cache
    type: cache
    provider:
      name: custom:Memcached
      host: cache.internal
      port: 11211
      tls: true
      ca_cert: certs/cache-ca.pem
```

Entity QML that called the bundled in-memory cache through the `Cache` helper keeps
calling it unchanged. That is what the family interface gives you: `Cache.get("session:42")`
does not know what answered.

## Try it, then think

> [!QUESTION]
> A connect point slot caches a per-user value with `Cache.set("profile:" + name, ...)`,
> where `name` is a display name the user chose. What display name could a user pick, and
> what does the adaptor above do about it? Now suppose `wireKey()` were written as
> `key.toUtf8()`.

<details class="solution" markdown>
<summary>Solution</summary>

A display name with a space, such as `alice 0 0 6`, makes the key `profile:alice 0 0 6`.
The memcached protocol is line based and space separated, so everything after the space
is read as the command's arguments. With `key.toUtf8()`, the user writes memcached
commands, and with a carefully chosen name can overwrite or expire other users' entries.

`wireKey()` percent-encodes, so the key becomes `profile%3Aalice%200%200%206`: one token,
no spaces, no control characters, and reversible, so two different names stay two
different keys. It also refuses keys over the engine's 250 byte limit, instead of sending
a command the engine would reject ambiguously.

This is the SQL injection lesson in another protocol, and it applies everywhere: any
adaptor whose engine has a syntax has an injection to prevent. Binding a parameter,
escaping a key or building a typed request all keep the same rule: a value never reaches
the engine where the engine expects syntax. The interfaces make that easy; you can only get
it wrong by assembling syntax yourself, which this adaptor must do, hence this function.

</details>

## What you learned

- An engine with no Qt driver is still one class. The family interface does not care
  whether a library sits behind it.
- The cache family's error model is lossy: a miss and a failure look the same, so a broken
  cache slows a system down instead of breaking it.
- A synchronous family interface means a blocking call on the entity's event loop, so it
  needs a short timeout that counts as a miss. An engine too slow for that belongs behind
  a connect point, not a cache interface.
- The engine's features constrain your encoding. The native atomic counter decided how
  integers are stored here, and the `flags` field kept everything else opaque.
- Where the engine and the interface disagree, close the gap honestly. Emulate what you can
  without losing a guarantee (create a missing counter with `add`), and document what you
  cannot (a counter that will not go negative).
- Any engine with a syntax has an injection risk. Encode at the boundary, once, in one
  function.

As on the last page: if you build this against a real engine, please
[send it](tutorial-advanced.md#when-yours-works-send-it).
