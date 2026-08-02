<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# A cache of your own

[The database page](tutorial-advanced-database.md) had Qt doing most of the work: a
driver existed, so the adaptor was a connection and a dialect. This page is the other
shape. Memcached has no Qt driver, no Qt module, and no client library SynQt pulls in, so
the adaptor is the protocol itself, written by hand over a socket.

That is a good thing to have done once. Most engines worth adapting are in this shape,
and the protocol is usually the easy part: what takes the thought is what to do where the
engine and the interface disagree, and Memcached disagrees in three interesting places.

## Step 1: A smaller interface, and a different error model

The cache family is `ICacheProvider`, and it is nine functions:

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

Look at what is missing. `set`, `del`, and `expire` return nothing, and `get` has no way
to report a failure: an absent value and an unreachable engine both come back as an
invalid `QVariant`. That is not an oversight, it is the family's contract. A cache is an
optimization, so a miss is a normal result and a broken cache is a slow system rather
than a broken one. Callers are entitled to ignore the difference, which means your adaptor
must never turn a cache problem into an application problem: no throwing, no blocking
forever, no returning stale data it is not sure about.

`incr` is the exception that returns something, and it is where this engine gets
interesting.

## Step 2: Scaffold and connect

```cli
synqt add provider Memcached --family cache
```

Then the connection. Memcached listens on 11211 and speaks a line-based text protocol.
It supports TLS only in builds configured for it, which does not soften the rule: an
unverified connection to a real address is refused in a release build, here as everywhere.

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
    // An integer is stored as its decimal text and nothing else. This is not a style
    // choice: memcached's incr parses the stored bytes itself, so the moment you want the
    // engine's atomic counter, the engine dictates how numbers are written.
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
// version of binding a SQL parameter: the caller's bytes never reach the engine as syntax.
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
            // Dev on loopback only; the check above already refused anything else.
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

Note what the socket is not doing: it is not asynchronous. Every exchange here blocks, for
at most a quarter second, on the entity's own event loop. That is the right trade for a
cache lookup that normally takes under a millisecond on a private network, and it is why
the timeout is short and treated as a miss rather than retried. If your engine's typical
answer is not that fast, it does not belong behind a synchronous family interface, and the
honest place for it is an entity of its own with a connect point.

## Step 3: The protocol

Two helpers carry every command. They are the whole of the wire handling:

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
    // line: the payload is arbitrary bytes and may contain a newline of its own.
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

`get` and `set` are then almost transcriptions of the protocol:

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
            readLine();  // STORED; a cache write that failed is a cache miss later
        }
    }

    void del(const QString &key) override
    {
        const QByteArray wire{wireKey(key)};
        if (!wire.isEmpty() && writeAll("delete " + wire + "\r\n")) {
            readLine();  // DELETED or NOT_FOUND; neither is the caller's problem
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

Here is where an adaptor stops being transcription. Memcached and `ICacheProvider` do not
agree about counters, and there are exactly three gaps.

It will not create the counter. `incr` on a key that does not exist returns
`NOT_FOUND`; it does not start at zero. The interface promises to return the new value, so
"the key was missing" is not an answer you may pass upwards. The fix is `add`, which
stores only if the key is still absent, so the race with another entity doing the same
thing at the same moment resolves rather than corrupting: whoever loses the `add` simply
increments what the winner created.

It only counts up. Memcached has `incr` and a separate `decr`, and `by` in the
interface is signed. Pick the command from the sign.

It floors at zero. `decr` past zero gives zero, not a negative number, and there is
nothing you can do about that from outside the engine. So say so, in the code, where
someone reaching for a counter that goes negative will read it. Documenting a limitation
is a real fix; hiding it behind a read-modify-write that is no longer atomic is not.

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
        // Lost the race. One retry, not a loop: if the key is missing again the engine is
        // not behaving, and a cache is allowed to give up rather than spin.
        const qint64 retried{step(wire, by)};
        return retried >= 0 ? retried : 0;
    }

private:
    /// One incr/decr round trip. -1 means the key was missing or the exchange failed;
    /// memcached counters are unsigned, so a real answer is never negative.
    qint64 step(const QByteArray &wire, qint64 by)
    {
        // decr floors at zero: this engine has no negative counters, and emulating them
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
    kind: service
    blueprint: cache
    provider:
      name: custom:Memcached
      host: cache.internal
      port: 11211
      tls: true
      ca_cert: certs/cache-ca.pem
```

Any entity QML that was calling the bundled in-memory cache through the `Cache` helper
keeps calling it. That is the point of the family: `Cache.get("session:42")` does not know
what answered.

## Try it, then think

> [!QUESTION]
> A connect point slot caches a per-user value with `Cache.set("profile:" + name, ...)`,
> where `name` is a display name the user chose. What could a user pick as their display
> name, and what would the adaptor above do about it? Now suppose `wireKey()` had been
> written as `key.toUtf8()`.

<details class="solution" markdown>
<summary>Solution</summary>

A display name containing a space, say `alice 0 0 6`, would make the key
`profile:alice 0 0 6`. The memcached protocol is line-based and space-separated, so that
key is not a key at all: the rest of it is read as the command's arguments. With
`key.toUtf8()` the user is writing memcached commands, and can overwrite or expire entries
belonging to other users by choosing a name carefully.

`wireKey()` percent-encodes, so the key becomes `profile%3Aalice%200%200%206`: one token,
no spaces, no control characters, and reversible so two different names stay two different
keys. It also refuses anything over the engine's 250 byte limit rather than sending a
command the engine will reject in an ambiguous way.

This is the SQL injection lesson in a different protocol, and it generalizes: any adaptor
whose engine has a syntax has an injection to prevent. Whether that is achieved by binding
a parameter, escaping a key, or building a typed request object, the invariant is the
same, which is that a value must never reach the engine where the engine expects syntax.
The interfaces are shaped to make that easy, so the only way to get it wrong is to
assemble the syntax yourself, which is what this adaptor does and why this function
exists.

</details>

## What you learned

- An engine with no Qt driver is still one class: the family interface does not care
  whether there is a library behind it.
- The cache family's error model is deliberately lossy. A miss and a failure look the
  same, on purpose, so a broken cache degrades a system instead of breaking it.
- A synchronous family interface means a blocking call on the entity's event loop, so it
  needs a short timeout and a treat-it-as-a-miss policy. An engine too slow for that
  belongs behind a connect point, not behind a cache interface.
- The engine's own features constrain your encoding. A native atomic counter dictated how
  integers are stored here, and the `flags` field is what let the rest stay opaque.
- Where the engine and the interface disagree, close the gap honestly: emulate what can
  be emulated without losing a guarantee (creating a missing counter with `add`), and
  document what cannot (a counter that will not go negative).
- Any engine with a syntax has an injection. Encode at the boundary, once, in one
  function.

The same invitation as the last page: if you have built this against a real engine,
[send it](tutorial-advanced.md#when-yours-works-send-it) rather than keeping it.
