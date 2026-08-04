// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A published model travels from the owner to its consumers and no further. QtRO's
// model replica exposes setData(), which the source-side adapter accepts as a slot on
// the wire; a write arriving that way never reaches the owner's QML, so no Caller and
// no owner-side rule ever sees it, and it fans out to every other consumer. These are
// the tests that say it does not happen.

#include "rows_rep.h"
#include "rows_sourcehelper.h"
#include "rows_replica.h"

#include <QAbstractItemModelReplica>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QTest>

class TestModelWrite : public QObject
{
    Q_OBJECT

private:
    static QVariantMap row(const QString &id, const QString &text)
    {
        return QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("text"), text}};
    }

    // Role indices follow the contract's declaration order, from Qt::UserRole.
    static constexpr int idRole{Qt::UserRole};
    static constexpr int textRole{Qt::UserRole + 1};

private slots:
    void initTestCase()
    {
        const QUrl url{QStringLiteral("local:m1modelwrite")};
        m_host.reset(new QRemoteObjectHost{url});
        m_source.reset(new RowsSourceHelper{});
        QVERIFY(m_host->enableRemoting<RowsSourceAPI>(m_source.data()));

        m_node.reset(new QRemoteObjectNode{url});
        m_replica.reset(m_node->acquire<RowsReplica>());
        QVERIFY(m_replica->waitForSource(3000));

        m_source->setRows(QVariantList{row(QStringLiteral("a"), QStringLiteral("first"))});
        m_model = m_replica->rows();
        QVERIFY(m_model != nullptr);
        QTRY_COMPARE(m_model->rowCount(), 1);
        QTRY_COMPARE(m_model->index(0, 0).data(textRole).toString(), QStringLiteral("first"));
    }

    void consumerCannotWriteTheModel()
    {
        const QModelIndex index{m_model->index(0, 0)};
        m_model->setData(index, QStringLiteral("rewritten"), textRole);

        // The write travelled and the owner refused it. Its own row is untouched, so
        // nothing was fanned out to any other consumer either.
        QTest::qWait(200);
        QCOMPARE(m_source->rows()->index(0, 0).data(textRole).toString(),
                 QStringLiteral("first"));

        // What setData returned is not asserted, and neither is what this one consumer
        // now shows itself. QtRO's model replica writes its own cache and returns true
        // before anything reaches the owner, so the return value is not an answer and
        // the owner cannot make it one. A consumer lying to its own view is its own
        // business; the boundary is the owner's state, above, and the next publish
        // replaces the lie (theOwnerCanStillPublish).
    }

    void theOwnerCanStillPublish()
    {
        m_source->setRows(QVariantList{row(QStringLiteral("b"), QStringLiteral("second"))});
        QTRY_COMPARE(m_model->index(0, 0).data(textRole).toString(),
                     QStringLiteral("second"));
        // QTRY: a role this consumer has not read yet arrives empty and is fetched.
        QTRY_COMPARE(m_model->index(0, 0).data(idRole).toString(), QStringLiteral("b"));
    }

    void theModelReportsItselfNotEditable()
    {
        QVERIFY(!(m_source->rows()->flags(m_source->rows()->index(0, 0))
                  & Qt::ItemIsEditable));
        // Flags replicate, so a view on the consumer side does not offer the edit either.
        QVERIFY(!(m_model->flags(m_model->index(0, 0)) & Qt::ItemIsEditable));
    }

    void cleanupTestCase()
    {
        m_replica.reset();
        m_node.reset();
        m_host.reset();
        m_source.reset();
    }

private:
    QScopedPointer<QRemoteObjectHost> m_host;
    QScopedPointer<RowsSourceHelper> m_source;
    QScopedPointer<QRemoteObjectNode> m_node;
    QScopedPointer<RowsReplica> m_replica;
    QAbstractItemModelReplica *m_model{nullptr};
};

QTEST_GUILESS_MAIN(TestModelWrite)
#include "tst_modelwrite.moc"
