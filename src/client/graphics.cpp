// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "graphics.h"

#include "graphicsnotice.h"
#include "graphicsprobe.h"

#include <QByteArray>
#include <QLatin1String>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QUrl>
#include <QVariant>

#include <atomic>

namespace SynQt {

namespace {

/// The previously installed handler, so nothing stops being logged.
QtMessageHandler g_previousHandler{nullptr};

/// The instance the watcher reports to. Atomic, because a message handler runs on the
/// logging thread (the render thread on desktop).
std::atomic<Graphics *> g_watched{nullptr};

/// What Qt says when it will not draw something without an accelerated scene graph.
///
/// Matching Qt's wording rather than a type list also catches content the build never saw
/// (a Loader's page, an edge-delivered page). Because it matches strings, tests/graphics
/// instantiates each type under the raster adaptation and fails when a message changes.
///
/// qquickshadereffect.cpp also has a "No shader effect node" warning that is unreachable
/// here: QQuickShaderEffectPrivate::handleUpdatePaintNode returns early because the raster
/// adaptation has no QSGGuiThreadShaderEffectManager, so a ShaderEffect draws nothing and
/// logs nothing. Only the build-time scan covers it (see
/// tests/graphics/tst_softwarebackend.cpp).
const char *const refusals[]{
    // qtquick3d, qquick3dviewport.cpp. Any View3D on a non-RHI scene graph.
    "Qt Quick 3D is not functional in such an environment",
};

void watchingHandler(QtMsgType type, const QMessageLogContext &context,
                     const QString &message)
{
    Graphics *watched{g_watched.load(std::memory_order_acquire)};
    if (watched && Graphics::isRefusal(message)) {
        QMetaObject::invokeMethod(watched, "unsupportedContentFound",
                                  Qt::QueuedConnection);
    }
    if (g_previousHandler) {
        g_previousHandler(type, context, message);
    }
}

} // namespace

Graphics::Graphics(QObject *parent)
    : QObject{parent}
{
    connect(this, &Graphics::unsupportedContentFound,
            this, &Graphics::reportUnsupportedContent);
}

Graphics::~Graphics()
{
    Graphics *self{this};
    if (g_watched.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel)) {
        qInstallMessageHandler(g_previousHandler);
        g_previousHandler = nullptr;
    }
}

bool Graphics::isSoftwareRendered() const
{
    return GraphicsProbe::isSoftwareRendered();
}

bool Graphics::hasUnsupportedContent() const
{
    return m_unsupportedContent;
}

void Graphics::installWatcher()
{
    Graphics *unwatched{nullptr};
    if (!g_watched.compare_exchange_strong(unwatched, this, std::memory_order_acq_rel)) {
        return;
    }
    g_previousHandler = qInstallMessageHandler(watchingHandler);
}

bool Graphics::isRefusal(const QString &message)
{
    for (const char *const refusal : refusals) {
        if (message.contains(QLatin1String(refusal))) {
            return true;
        }
    }
    return false;
}

QQmlComponent *Graphics::noticeComponent(QQmlEngine *engine, const QString &overrideUrl,
                                         QObject *parent)
{
    if (!engine) {
        return nullptr;
    }
    if (!overrideUrl.isEmpty()) {
        return new QQmlComponent{engine, QUrl{overrideUrl}, parent};
    }
    auto *component{new QQmlComponent{engine, parent}};
    component->setData(QByteArray{graphicsNoticeSource()},
                       QUrl{QStringLiteral("qrc:/synqt/GraphicsNotice.qml")});
    return component;
}

void Graphics::attachTo(QObject *rootWindow, QQmlEngine *engine, const QString &noticeUrl)
{
    m_rootWindow = rootWindow;
    m_engine = engine;
    m_noticeUrl = noticeUrl;
    if (m_unsupportedContent) {
        showNotice();
    }
}

void Graphics::reportUnsupportedContent()
{
    const bool first{!m_unsupportedContent};
    m_unsupportedContent = true;
    if (first) {
        showNotice();
    }
}

void Graphics::showNotice()
{
    if (m_notice || !m_rootWindow || !m_engine) {
        return;
    }
    // The window's content item as a plain QObject, so the client runtime does not link Qt
    // Quick; the engine converts it to QQuickItem when setting the parent below.
    QObject *contentItem{m_rootWindow->property("contentItem").value<QObject *>()};
    if (!contentItem) {
        return;
    }
    QQmlComponent *component{noticeComponent(m_engine, m_noticeUrl, this)};
    if (!component || component->isError()) {
        qWarning("SynQt: the graphics notice could not be loaded");
        delete component;
        return;
    }
    // Initial properties rather than a later write: the notice anchors to its parent, and a
    // binding against a null parent would not re-anchor cleanly.
    m_notice = component->createWithInitialProperties(
        {{QStringLiteral("parent"), QVariant::fromValue(contentItem)}});
    if (!m_notice) {
        qWarning("SynQt: the graphics notice could not be created");
    }
    component->deleteLater();
}

} // namespace SynQt
