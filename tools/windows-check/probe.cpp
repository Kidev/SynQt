// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The smallest possible target that exercises the whole cross-compile chain: clang-cl
// with the xwin CRT and SDK, linking a real Qt module (Core) from the Windows kit, with
// the code generators run from the Linux host kit. If this links to a PE .exe, the
// toolchain in cmake/toolchains/windows-clang-cl.cmake is sound and the check can move
// on to the real framework targets. It is a console app on purpose (no WIN32), so the
// verdict is a normal stdout line, and it touches a Q_OS_WIN block so the Windows path
// is compiled, not just skipped.

#include <QCoreApplication>
#include <QtGlobal>

#include <cstdio>

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};

#ifdef Q_OS_WIN
    const char *const platform{"windows"};
#else
    const char *const platform{"not-windows"};
#endif

    std::printf("synqt windows-check probe: qt=%s platform=%s\n",
                qVersion(), platform);
    return 0;
}
