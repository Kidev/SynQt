// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "sqlconnectionpool.h"

#include <QSqlError>

#include <utility>

namespace SynQt {

SqlConnectionPool::Lease::Lease(SqlConnectionPool *pool, int slot)
    : m_pool{pool}
    , m_slot{slot}
{
}

SqlConnectionPool::Lease::~Lease()
{
    if (m_pool != nullptr && m_slot >= 0) {
        m_pool->release(m_slot);
    }
}

SqlConnectionPool::Lease::Lease(Lease &&other) noexcept
    : m_pool{other.m_pool}
    , m_slot{other.m_slot}
{
    other.m_pool = nullptr;
    other.m_slot = -1;
}

SqlConnectionPool::Lease &SqlConnectionPool::Lease::operator=(Lease &&other) noexcept
{
    if (this != &other) {
        if (m_pool != nullptr && m_slot >= 0) {
            m_pool->release(m_slot);
        }
        m_pool = other.m_pool;
        m_slot = other.m_slot;
        other.m_pool = nullptr;
        other.m_slot = -1;
    }
    return *this;
}

QSqlDatabase &SqlConnectionPool::Lease::database()
{
    return m_pool->m_slots[m_slot].db;
}

void SqlConnectionPool::Lease::markBroken()
{
    if (m_pool != nullptr && m_slot >= 0) {
        m_pool->setBroken(m_slot);
    }
}

SqlConnectionPool::SqlConnectionPool(QString driver, Configure configure, int maxSize)
    : m_driver{std::move(driver)}
    , m_configure{std::move(configure)}
    , m_maxSize{maxSize > 0 ? maxSize : 1}
{
    // Every slot the pool will ever hold, allocated now, so the list never reallocates.
    //
    // Not a performance note. `Lease::database()` hands out a `QSqlDatabase &` into this
    // list, and a lease is held for as long as its caller is running a query. The pool grows
    // on demand, under a mutex a lease does not hold, so a second caller asking for a
    // connection while the first is using one would move the list and leave that reference
    // pointing at freed memory. Reserving the cap is what makes the reference outlive the
    // growth, since the size can never pass `m_maxSize`.
    m_slots.reserve(m_maxSize);
}

SqlConnectionPool::~SqlConnectionPool()
{
    closeAll();
}

bool SqlConnectionPool::openSlot(Slot &slot, QString *error)
{
    if (m_configure) {
        m_configure(slot.db);
    }
    if (!slot.db.open()) {
        if (error != nullptr) {
            *error = slot.db.lastError().text();
        }
        return false;
    }
    slot.broken = false;
    return true;
}

SqlConnectionPool::Lease SqlConnectionPool::acquire(QString *error)
{
    QMutexLocker locker{&m_mutex};

    // Reuse an idle connection. Reopen it first if it was found dead or flagged broken.
    for (int index{0}; index < m_slots.size(); ++index) {
        Slot &slot{m_slots[index]};
        if (slot.leased) {
            continue;
        }
        if (slot.broken || !slot.db.isOpen()) {
            slot.db.close();
            if (!openSlot(slot, error)) {
                continue;  // this connection will not come back. Try another / grow
            }
        }
        slot.leased = true;
        return Lease{this, index};
    }

    // No idle connection. Grow the pool up to the cap.
    if (m_slots.size() < m_maxSize) {
        const QString name{QStringLiteral("synqt-pool-%1-%2")
                               .arg(m_driver.toLower())
                               .arg(++m_counter)};
        Slot slot;
        slot.db = QSqlDatabase::addDatabase(m_driver, name);
        if (!openSlot(slot, error)) {
            QSqlDatabase::removeDatabase(name);
            return Lease{};
        }
        m_slots.append(std::move(slot));
        m_slots.last().leased = true;
        // The slot index is an int throughout the pool (bounded by maxSize). The just-grown
        // list cannot exceed it, so the narrowing from qsizetype is safe.
        return Lease{this, static_cast<int>(m_slots.size() - 1)};
    }

    if (error != nullptr) {
        *error = QStringLiteral("connection pool exhausted (size %1)").arg(m_maxSize);
    }
    return Lease{};
}

void SqlConnectionPool::release(int slot)
{
    QMutexLocker locker{&m_mutex};
    if (slot >= 0 && slot < m_slots.size()) {
        m_slots[slot].leased = false;
    }
}

void SqlConnectionPool::setBroken(int slot)
{
    QMutexLocker locker{&m_mutex};
    if (slot >= 0 && slot < m_slots.size()) {
        m_slots[slot].broken = true;
    }
}

void SqlConnectionPool::closeAll()
{
    QMutexLocker locker{&m_mutex};
    for (Slot &slot : m_slots) {
        const QString name{slot.db.connectionName()};
        if (slot.db.isOpen()) {
            slot.db.close();
        }
        slot.db = QSqlDatabase{};
        if (!name.isEmpty() && QSqlDatabase::contains(name)) {
            QSqlDatabase::removeDatabase(name);
        }
    }
    m_slots.clear();
    // And reserved again, because clear() is not obliged to keep the capacity the
    // constructor asked for and this pool may be used after being closed: `acquire()` grows
    // it back on demand. The reservation is what keeps `Lease::database()`'s reference into
    // this list valid while another caller is growing it (see the constructor), so it has to
    // hold for the pool's whole life and not only until the first disconnect.
    m_slots.reserve(m_maxSize);
}

int SqlConnectionPool::openCount() const
{
    QMutexLocker locker{&m_mutex};
    return static_cast<int>(m_slots.size());
}

int SqlConnectionPool::busyCount() const
{
    QMutexLocker locker{&m_mutex};
    int busy{0};
    for (const Slot &slot : m_slots) {
        if (slot.leased) {
            ++busy;
        }
    }
    return busy;
}

} // namespace SynQt
