// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SQLCONNECTIONPOOL_H
#define SYNQT_SQLCONNECTIONPOOL_H

#include <QList>
#include <QMutex>
#include <QSqlDatabase>
#include <QString>

#include <functional>

namespace SynQt {

/// A bounded pool of QSqlDatabase connections for an external relational provider
/// (postgres, mysql). It is engine-agnostic: the driver name and a Configure hook that
/// stamps host/database/user/password/TLS options onto a fresh connection are supplied by
/// the provider, so the same pool backs every client/server engine. It grows lazily up to
/// maxSize, reuses released connections, reopens a connection found dead on acquire, and
/// refuses to exceed the cap. A connection is owned by the thread that created the pool
/// (Qt SQL requires a connection be used only on its creating thread); the mutex guards the
/// pool's own bookkeeping so lease/release remains correct if that thread also drives an
/// event loop.
class SqlConnectionPool
{
public:
    /// Stamps the connection settings (host, database, user, password, connect options)
    /// onto a newly added QSqlDatabase, immediately before it is opened.
    using Configure = std::function<void(QSqlDatabase &)>;

    /// An RAII handle to one leased connection. It returns the connection to the pool on
    /// destruction. Move-only, so a lease is held by exactly one owner at a time.
    class Lease
    {
    public:
        Lease() = default;
        Lease(SqlConnectionPool *pool, int slot);
        ~Lease();
        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;

        bool isValid() const { return m_pool != nullptr && m_slot >= 0; }
        QSqlDatabase &database();

        /// Flag this connection as broken (e.g. the engine dropped it mid-statement) so the
        /// pool discards and reopens it on the next acquire rather than handing it out again.
        void markBroken();

    private:
        SqlConnectionPool *m_pool{nullptr};
        int m_slot{-1};
    };

    SqlConnectionPool(QString driver, Configure configure, int maxSize);
    ~SqlConnectionPool();

    SqlConnectionPool(const SqlConnectionPool &) = delete;
    SqlConnectionPool &operator=(const SqlConnectionPool &) = delete;

    /// Lease a connection: reuse an idle open one, else grow to a new one up to maxSize,
    /// else fail with *error set ("pool exhausted"). A dead idle connection is reopened.
    Lease acquire(QString *error);

    /// Close and drop every connection (used on disconnect).
    void closeAll();

    int openCount() const;   // connections currently created (leased or idle)
    int busyCount() const;   // connections currently leased out
    int maxSize() const { return m_maxSize; }

private:
    friend class Lease;

    struct Slot
    {
        QSqlDatabase db;
        bool leased{false};
        bool broken{false};
    };

    void release(int slot);
    void setBroken(int slot);
    bool openSlot(Slot &slot, QString *error);

    QString m_driver;
    Configure m_configure;
    int m_maxSize;
    QList<Slot> m_slots;
    mutable QMutex m_mutex;
    int m_counter{0};
};

} // namespace SynQt

#endif // SYNQT_SQLCONNECTIONPOOL_H
