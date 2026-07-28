// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "envfile.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QTextStream>

namespace SynQt {

namespace {

// Strip one matching pair of surrounding quotes, so `KEY="a value"` carries the value and
// not the quotes. Only a matching pair: a value that opens a quote and never closes it is
// taken literally rather than half-eaten.
QString unquoted(const QString &value)
{
    if (value.size() < 2) {
        return value;
    }
    const QChar first{value.front()};
    if ((first == u'"' || first == u'\'') && value.back() == first) {
        return value.mid(1, value.size() - 2);
    }
    return value;
}

} // namespace

EnvFileResult loadEnvFile(const QString &path, int *loadedCount)
{
    if (loadedCount) {
        *loadedCount = 0;
    }
    if (path.isEmpty()) {
        return EnvFileResult::NotFound;
    }
    QFileInfo info{path};
    if (!info.exists()) {
        return EnvFileResult::NotFound;
    }
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning().noquote() << "cannot read env file" << path << ":" << file.errorString();
        return EnvFileResult::Unreadable;
    }

    int loaded{0};
    int lineNumber{0};
    QTextStream stream{&file};
    while (!stream.atEnd()) {
        ++lineNumber;
        const QString line{stream.readLine().trimmed()};
        if (line.isEmpty() || line.startsWith(u'#')) {
            continue;
        }
        // A line pasted out of a shell script keeps its `export `; take the assignment
        // rather than treating the whole thing as a variable named "export FOO".
        const QString assignment{line.startsWith(QLatin1String("export "))
                                 ? line.mid(7).trimmed() : line};
        const qsizetype separator{assignment.indexOf(u'=')};
        if (separator <= 0) {
            // Name the line, never its contents: this file holds secrets, and a
            // diagnostic that quotes a malformed line is a diagnostic that leaks one.
            qWarning().noquote() << "env file" << path << "line" << lineNumber
                                 << "is not KEY=VALUE; ignored";
            continue;
        }
        const QByteArray key{assignment.left(separator).trimmed().toUtf8()};
        if (key.isEmpty() || qEnvironmentVariableIsSet(key.constData())) {
            // Already set wins: the deployment's own environment is the authority, and
            // the file only fills what it left unset.
            continue;
        }
        const QString value{unquoted(assignment.mid(separator + 1).trimmed())};
        if (qputenv(key.constData(), value.toUtf8())) {
            ++loaded;
        }
    }

    if (loadedCount) {
        *loadedCount = loaded;
    }
    return EnvFileResult::Loaded;
}

} // namespace SynQt
