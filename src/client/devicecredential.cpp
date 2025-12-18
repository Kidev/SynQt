// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "devicecredential.h"

#include <QCoreApplication>
#include <QSysInfo>

#include <chrono>
#include <functional>
#include <future>
#include <thread>

namespace SynQt {

namespace {

/// How long any one store call may take before it is abandoned.
///
/// Two seconds because this runs before the first frame. A keyring that is going to answer
/// answers in microseconds, and one that does not is either prompting (which must not happen
/// at startup) or wedged on a bus that is not there. Either way the right outcome is the same
/// as having no store, which costs the visitor one sign-in and costs nobody a hung window.
constexpr int kDeadlineMs{2000};

/// What a call writes, in memory both the caller and an abandoned worker can hold.
struct Outcome
{
    bool ok{false};
    QByteArray secret;
    QString error;
};

/// Run one store call with a deadline, on a thread of its own.
///
/// The awkward shape is the point. On a timeout the worker is detached rather than joined,
/// because it is stuck inside a platform API with no cancellation, and the two things it
/// could still touch (the store and the outcome) are both shared with it so neither can be
/// freed underneath it. Nothing reads the outcome after a timeout, so there is no race for
/// the value either. The caller has already decided this machine has no store.
std::shared_ptr<Outcome> runWithDeadline(
    const std::shared_ptr<SecureStore> &store,
    const std::function<void(SecureStore &, Outcome &)> &call, bool *timedOut)
{
    auto outcome{std::make_shared<Outcome>()};
    auto finished{std::make_shared<std::promise<void>>()};
    std::future<void> answer{finished->get_future()};
    std::thread worker{[store, call, outcome, finished]() {
        call(*store, *outcome);
        finished->set_value();
    }};
    worker.detach();

    const bool ready{answer.wait_for(std::chrono::milliseconds(kDeadlineMs))
                     == std::future_status::ready};
    if (timedOut) {
        *timedOut = !ready;
    }
    return ready ? outcome : std::make_shared<Outcome>();
}

/// The two halves as one blob. A separator no hex token can contain, so the split cannot be
/// confused by either half's content.
QByteArray packed(const DeviceCredential::Held &held)
{
    return held.id.toUtf8() + '\n' + held.secret;
}

DeviceCredential::Held unpacked(const QByteArray &blob)
{
    const qsizetype separator{blob.indexOf('\n')};
    if (separator <= 0) {
        return DeviceCredential::Held{};
    }
    DeviceCredential::Held held;
    held.id = QString::fromUtf8(blob.left(separator));
    held.secret = blob.mid(separator + 1);
    return held;
}

} // namespace

DeviceCredential::DeviceCredential(const QUrl &edgeUrl, QObject *parent)
    : DeviceCredential{edgeUrl, makeSecureStore(), parent}
{
}

DeviceCredential::DeviceCredential(const QUrl &edgeUrl, std::unique_ptr<SecureStore> store,
                                   QObject *parent)
    : QObject{parent}
    , m_store{std::move(store)}
{
    QUrl origin{edgeUrl};
    origin.setScheme(origin.scheme() == QLatin1String("wss") ? QStringLiteral("https")
                                                             : QStringLiteral("http"));
    origin.setPath(QString{});
    m_account = origin.toString(QUrl::RemovePath);

    QString reason;
    m_available = m_store->isAvailable(&reason);
    if (!m_available) {
        // Said once, plainly, and not as a warning. On a headless session or a machine with
        // no keyring this is the ordinary state of affairs, and the only thing it costs is
        // a sign-in per launch. What would deserve a warning is the opposite.
        qInfo("SynQt: no secure store is available (%s), so this app will not stay signed in "
              "between launches. Nothing is written to disk.", qUtf8Printable(reason));
    }
}

DeviceCredential::~DeviceCredential() = default;

bool DeviceCredential::isAvailable() const
{
    return m_available;
}

SecureStore::Binding DeviceCredential::binding() const
{
    return m_available ? m_store->binding() : SecureStore::Binding::None;
}

QString DeviceCredential::bindingName() const
{
    return secureStoreBindingName(binding());
}

QString DeviceCredential::storeName() const
{
    return m_store->name();
}

DeviceCredential::Held DeviceCredential::load()
{
    if (!m_available) {
        return Held{};
    }
    const QString account{m_account};
    bool timedOut{false};
    const std::shared_ptr<Outcome> outcome{
        runWithDeadline(m_store, [account](SecureStore &store, Outcome &result) {
            result.ok = store.load(account, &result.secret, &result.error);
        }, &timedOut)};
    if (timedOut) {
        m_available = false;
        qWarning("SynQt: the secure store did not answer within %d ms, so this launch signs "
                 "in as if nothing were stored.", kDeadlineMs);
        return Held{};
    }
    if (!outcome->ok) {
        if (!outcome->error.isEmpty()) {
            qWarning("SynQt: could not read the stored sign-in (%s).",
                     qUtf8Printable(outcome->error));
        }
        return Held{};
    }
    const Held held{unpacked(outcome->secret)};
    outcome->secret.fill('\0');
    outcome->secret.clear();
    return held;
}

bool DeviceCredential::save(const Held &held)
{
    if (!m_available || !held.isValid()) {
        return false;
    }
    const QString account{m_account};
    QByteArray blob{packed(held)};
    bool timedOut{false};
    const std::shared_ptr<Outcome> outcome{
        runWithDeadline(m_store, [account, blob](SecureStore &store, Outcome &result) {
            QByteArray payload{blob};
            result.ok = store.store(account, payload, &result.error);
            payload.fill('\0');
        }, &timedOut)};
    blob.fill('\0');
    blob.clear();
    if (timedOut) {
        // Same reason as the failure below, plus one this path has of its own: nothing here
        // knows whether the write landed. A store that answered too late may have taken the
        // new generation or kept the old one, so what is on disk is a coin flip, and one of
        // its faces is a retired credential the edge would read as a second copy. Erase
        // before writing the store off, on the same deadline, so neither face is left.
        erase();
        m_available = false;
        return false;
    }
    if (!outcome->ok) {
        // Worth saying: the visitor asked to stay signed in and will not be. It is not fatal,
        // and in particular it is never a reason to fail the sign-in that just succeeded.
        qWarning("SynQt: could not store the sign-in for the next launch (%s).",
                 qUtf8Printable(outcome->error));
        // A store that reads but cannot write is the one shape of failure that is worse than
        // having no store at all. What it leaves behind is the generation this call was
        // replacing, which the edge has already retired, so the next launch would present a
        // second copy of a spent credential. The exact signature of a stolen one, answered by
        // revoking the family and every session on it. Leave nothing instead. The visitor
        // signs in again next launch, which is what a machine with no store does anyway.
        erase();
    }
    return outcome->ok;
}

void DeviceCredential::erase()
{
    if (!m_available) {
        return;
    }
    const QString account{m_account};
    bool timedOut{false};
    const std::shared_ptr<Outcome> outcome{
        runWithDeadline(m_store, [account](SecureStore &store, Outcome &result) {
            result.ok = store.erase(account, &result.error);
        }, &timedOut)};
    if (!timedOut && !outcome->ok) {
        qWarning("SynQt: could not remove the stored sign-in (%s). It may still be redeemable "
                 "from this machine until it expires.", qUtf8Printable(outcome->error));
    }
}

QString DeviceCredential::machineLabel()
{
    const QString host{QSysInfo::machineHostName()};
    const QString application{QCoreApplication::applicationName()};
    if (host.isEmpty()) {
        return application;
    }
    return application.isEmpty() ? host : (application + QStringLiteral(" on ") + host);
}

} // namespace SynQt
