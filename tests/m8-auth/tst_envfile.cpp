// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The env file is where an `env:` reference is answered: synqt.yaml carries the NAME of a
// secret (client_secret: env:GITHUB_CLIENT_SECRET) and the value arrives here, at startup,
// from the entity's own environment. What is tested is the part that decides whether a
// deployment is safe rather than merely convenient: an existing variable is never
// overwritten (the real secret store outranks a file that may have been copied along with
// the binary), a missing file is the ordinary case and not a failure, and a malformed line
// is reported without its contents.

#include "envfile.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

using namespace SynQt;

class tst_EnvFile : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void loadsKeyValuePairs();
    void neverOverwritesTheRealEnvironment();
    void aMissingFileIsNotAFailure();
    void stripsCommentsQuotesAndExportPrefixes();
    void reportsAMalformedLineWithoutItsContents();

private:
    QString write(const QString &contents);

    QTemporaryDir m_dir;
    QStringList m_planted;
};

void tst_EnvFile::init()
{
    QVERIFY(m_dir.isValid());
    m_planted.clear();
}

void tst_EnvFile::cleanup()
{
    // Every variable this test set leaves with it: a leaked one would make the next case
    // pass for the wrong reason (the "never overwrites" rule would hide a load that failed).
    for (const QString &name : m_planted) {
        qunsetenv(name.toUtf8().constData());
    }
    for (const QString &file : QDir{m_dir.path()}.entryList(QDir::Files)) {
        QFile::remove(m_dir.filePath(file));
    }
}

QString tst_EnvFile::write(const QString &contents)
{
    const QString path{m_dir.filePath(QStringLiteral(".env"))};
    QFile file{path};
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return QString{};
    }
    file.write(contents.toUtf8());
    file.close();
    return path;
}

void tst_EnvFile::loadsKeyValuePairs()
{
    m_planted = {QStringLiteral("SYNQT_TST_SECRET"), QStringLiteral("SYNQT_TST_DB")};
    const QString path{write(QStringLiteral("SYNQT_TST_SECRET=s3cr3t\n"
                                            "SYNQT_TST_DB=postgres://localhost/app\n"))};
    QVERIFY(!path.isEmpty());

    int loaded{0};
    QCOMPARE(loadEnvFile(path, &loaded), EnvFileResult::Loaded);
    QCOMPARE(loaded, 2);
    QCOMPARE(qEnvironmentVariable("SYNQT_TST_SECRET"), QStringLiteral("s3cr3t"));
    QCOMPARE(qEnvironmentVariable("SYNQT_TST_DB"),
             QStringLiteral("postgres://localhost/app"));
}

void tst_EnvFile::neverOverwritesTheRealEnvironment()
{
    // The rule that makes this safe to call unconditionally at startup. A container, a
    // systemd unit or a CI secret store sets the variable; a stale .env that travelled
    // with the build directory must not be able to replace it with an old credential.
    m_planted = {QStringLiteral("SYNQT_TST_SECRET")};
    qputenv("SYNQT_TST_SECRET", QByteArrayLiteral("from-the-environment"));
    const QString path{write(QStringLiteral("SYNQT_TST_SECRET=from-the-file\n"))};

    int loaded{-1};
    QCOMPARE(loadEnvFile(path, &loaded), EnvFileResult::Loaded);
    QCOMPARE(loaded, 0);
    QCOMPARE(qEnvironmentVariable("SYNQT_TST_SECRET"),
             QStringLiteral("from-the-environment"));
}

void tst_EnvFile::aMissingFileIsNotAFailure()
{
    // Both generated mains call this unconditionally, and a deployment with a real secret
    // store has no file at all. NotFound has to be the quiet, ordinary answer, or every
    // correctly deployed entity would log a warning it can do nothing about.
    int loaded{-1};
    QCOMPARE(loadEnvFile(m_dir.filePath(QStringLiteral("absent.env")), &loaded),
             EnvFileResult::NotFound);
    QCOMPARE(loaded, 0);
    QCOMPARE(loadEnvFile(QString{}, &loaded), EnvFileResult::NotFound);
}

void tst_EnvFile::stripsCommentsQuotesAndExportPrefixes()
{
    m_planted = {QStringLiteral("SYNQT_TST_QUOTED"), QStringLiteral("SYNQT_TST_SINGLE"),
                 QStringLiteral("SYNQT_TST_EXPORTED"), QStringLiteral("SYNQT_TST_EMPTY"),
                 QStringLiteral("SYNQT_TST_UNBALANCED"), QStringLiteral("SYNQT_TST_URL")};
    const QString path{write(QStringLiteral(
        "# a comment\n"
        "\n"
        "SYNQT_TST_QUOTED=\"a value\"\n"
        "SYNQT_TST_SINGLE='a value'\n"
        "export SYNQT_TST_EXPORTED=exported\n"
        "SYNQT_TST_EMPTY=\n"
        "SYNQT_TST_UNBALANCED=\"half\n"
        // A value with its own '=' keeps every one of them: only the FIRST separates.
        "SYNQT_TST_URL=postgres://user:p=ss@host/db\n"))};

    QCOMPARE(loadEnvFile(path), EnvFileResult::Loaded);
    QCOMPARE(qEnvironmentVariable("SYNQT_TST_QUOTED"), QStringLiteral("a value"));
    QCOMPARE(qEnvironmentVariable("SYNQT_TST_SINGLE"), QStringLiteral("a value"));
    QCOMPARE(qEnvironmentVariable("SYNQT_TST_EXPORTED"), QStringLiteral("exported"));
    QVERIFY(qEnvironmentVariableIsSet("SYNQT_TST_EMPTY"));
    QCOMPARE(qEnvironmentVariable("SYNQT_TST_EMPTY"), QString{});
    // Only a MATCHING pair is stripped, so a value that opens a quote and never closes it
    // arrives whole rather than half-eaten.
    QCOMPARE(qEnvironmentVariable("SYNQT_TST_UNBALANCED"), QStringLiteral("\"half"));
    QCOMPARE(qEnvironmentVariable("SYNQT_TST_URL"),
             QStringLiteral("postgres://user:p=ss@host/db"));
}

void tst_EnvFile::reportsAMalformedLineWithoutItsContents()
{
    // A silently skipped line in a secrets file is a secret that is silently absent, so it
    // is reported. It is reported by LINE NUMBER: a diagnostic that quoted the line would
    // put the credential it failed to parse into the log.
    m_planted = {QStringLiteral("SYNQT_TST_GOOD")};
    const QString path{write(QStringLiteral("SYNQT_TST_GOOD=fine\n"
                                            "this-line-has-no-equals-sign\n"))};

    QTest::ignoreMessage(QtWarningMsg,
                         QRegularExpression{QStringLiteral("line 2 is not KEY=VALUE")});
    int loaded{0};
    QCOMPARE(loadEnvFile(path, &loaded), EnvFileResult::Loaded);
    QCOMPARE(loaded, 1);
    QCOMPARE(qEnvironmentVariable("SYNQT_TST_GOOD"), QStringLiteral("fine"));
}

QTEST_MAIN(tst_EnvFile)

#include "tst_envfile.moc"
