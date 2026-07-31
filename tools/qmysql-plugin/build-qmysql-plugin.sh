#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Build Qt's QMYSQL SQL driver plugin against MariaDB Connector/C, which is the only
# build of it SynQt's `mysql` persistence provider can legally use.
#
# Why this script exists at all. The Qt online installer ships a prebuilt
# plugins/sqldrivers/libqsqlmysql.so, and it is the wrong one twice over:
#
#   1. Legally. It is linked against Oracle's libmysqlclient, which is GPLv2-only and
#      therefore license-incompatible with the LGPLv3 Qt modules in the same conveyed
#      entity: an entity linking both cannot be distributed at all (docs/licensing.md).
#      SynQt's rule is MariaDB Connector/C (LGPLv2.1), never libmysqlclient.
#   2. Practically. It does not merely need `libmysqlclient.so.21` present; it needs
#      Oracle's versioned symbols (`libmysqlclient_21.0`). MariaDB Connector/C does not
#      export those, so no symlink or LD_LIBRARY_PATH shim can bridge the two. Point the
#      prebuilt plugin at Connector/C and Qt reports the driver as simply "not loaded",
#      naming nothing. Rebuilding is the only path.
#
# What you get: a libqsqlmysql.so linked against libmariadb.so, installed into a private
# plugin root, which Qt picks up through QT_PLUGIN_PATH without touching the installed
# Qt kit (no sudo, nothing to undo).
#
# Requirements: the Qt *source* tree for the pinned version (the online installer's "Qt
# 6.11.1 > Sources" component) and MariaDB Connector/C's headers and library.
#   Arch:   pacman -S mariadb-libs
#   Debian: apt install libmariadb-dev
#   Fedora: dnf install mariadb-connector-c-devel
#
# Environment:
#   QT_HOST      the Qt kit whose qt-cmake drives the build
#                (default: /opt/Qt/6.11.1/gcc_64)
#   QT_SRC       the Qt source tree (default: <QT_HOST>/../Src)
#   MYSQL_INCLUDE_DIR  where mysql.h lives (default: probed via pkg-config, then
#                /usr/include/mysql, then /usr/include/mariadb)
#   MYSQL_LIBRARY      the Connector/C shared library
#                (default: probed via pkg-config, then /usr/lib/libmariadb.so)
#   PLUGIN_ROOT  where to install (default: $HOME/.cache/synqt-qmysql)
#
#   tools/qmysql-plugin/build-qmysql-plugin.sh
#   export QT_PLUGIN_PATH="$HOME/.cache/synqt-qmysql"   # then run your entity or the tests

set -euo pipefail

QT_HOST="${QT_HOST:-/opt/Qt/6.11.1/gcc_64}"
QT_SRC="${QT_SRC:-$(cd "$QT_HOST/.." 2>/dev/null && pwd)/Src}"
PLUGIN_ROOT="${PLUGIN_ROOT:-$HOME/.cache/synqt-qmysql}"
BUILD_DIR="${BUILD_DIR:-$PLUGIN_ROOT/build}"

sqldrivers="$QT_SRC/qtbase/src/plugins/sqldrivers"
if [ ! -f "$sqldrivers/CMakeLists.txt" ]; then
    echo "no Qt sqldrivers sources at $sqldrivers" >&2
    echo "install the Qt 'Sources' component for this version, or set QT_SRC." >&2
    exit 1
fi
if [ ! -x "$QT_HOST/bin/qt-cmake" ]; then
    echo "no qt-cmake in $QT_HOST/bin (set QT_HOST to your Qt kit)" >&2
    exit 1
fi

# Locate Connector/C. pkg-config is authoritative where the package ships a .pc file;
# the fallbacks cover the distributions that do not.
if [ -z "${MYSQL_INCLUDE_DIR:-}" ] && command -v pkg-config >/dev/null 2>&1; then
    if pkg-config --exists libmariadb; then
        MYSQL_INCLUDE_DIR="$(pkg-config --variable=includedir libmariadb)/mysql"
        [ -f "$MYSQL_INCLUDE_DIR/mysql.h" ] || \
            MYSQL_INCLUDE_DIR="$(pkg-config --variable=includedir libmariadb)/mariadb"
    fi
fi
for candidate in "${MYSQL_INCLUDE_DIR:-}" /usr/include/mysql /usr/include/mariadb; do
    if [ -n "$candidate" ] && [ -f "$candidate/mysql.h" ]; then
        MYSQL_INCLUDE_DIR="$candidate"
        break
    fi
done
for candidate in "${MYSQL_LIBRARY:-}" /usr/lib/libmariadb.so /usr/lib64/libmariadb.so \
                 /usr/lib/x86_64-linux-gnu/libmariadb.so; do
    if [ -n "$candidate" ] && [ -e "$candidate" ]; then
        MYSQL_LIBRARY="$candidate"
        break
    fi
done
if [ -z "${MYSQL_INCLUDE_DIR:-}" ] || [ -z "${MYSQL_LIBRARY:-}" ]; then
    echo "MariaDB Connector/C not found; install it (see the header of this script)" >&2
    echo "or set MYSQL_INCLUDE_DIR and MYSQL_LIBRARY." >&2
    exit 1
fi

# Refuse to build against Oracle's client even if it is what was found: a plugin linked
# to it cannot be distributed with the LGPLv3 Qt modules, so producing one here would
# only postpone the problem to the deployment that cannot ship.
case "$(readlink -f "$MYSQL_LIBRARY")" in
    *mariadb*) ;;
    *) echo "refusing to build against $MYSQL_LIBRARY: it does not resolve to MariaDB" >&2
       echo "Connector/C. Oracle's libmysqlclient is GPLv2-only and cannot be conveyed" >&2
       echo "with the LGPLv3 Qt modules in the same entity (docs/licensing.md)." >&2
       exit 1 ;;
esac

echo "Qt kit:      $QT_HOST"
echo "Qt sources: $sqldrivers"
echo "Connector/C: $MYSQL_LIBRARY (headers: $MYSQL_INCLUDE_DIR)"
echo "installing: $PLUGIN_ROOT/sqldrivers"

generator=(-G Ninja)
command -v ninja >/dev/null 2>&1 || generator=()
"$QT_HOST/bin/qt-cmake" -S "$sqldrivers" -B "$BUILD_DIR" "${generator[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMySQL_INCLUDE_DIR="$MYSQL_INCLUDE_DIR" \
    -DMySQL_LIBRARY="$MYSQL_LIBRARY"

if ! grep -qi "MySql .* yes" "$BUILD_DIR/config.summary" 2>/dev/null; then
    echo "configure did not enable the MySql driver; see $BUILD_DIR/config.summary" >&2
    exit 1
fi

cmake --build "$BUILD_DIR" --target QMYSQLDriverPlugin

built="$BUILD_DIR/plugins/sqldrivers/libqsqlmysql.so"
[ -f "$built" ] || built="$BUILD_DIR/plugins/sqldrivers/qsqlmysql.dll"
[ -f "$built" ] || built="$BUILD_DIR/plugins/sqldrivers/libqsqlmysql.dylib"
if [ ! -f "$built" ]; then
    echo "the build produced no QMYSQL plugin under $BUILD_DIR/plugins/sqldrivers" >&2
    exit 1
fi

# Prove the linkage rather than trusting the configure summary: this is the check that
# would have caught the shipped plugin's Oracle dependency.
if command -v ldd >/dev/null 2>&1; then
    if ldd "$built" | grep -q "libmysqlclient"; then
        echo "the built plugin links libmysqlclient (Oracle); refusing to install it" >&2
        exit 1
    fi
    echo "linkage: $(ldd "$built" | grep -i mariadb | tr -s ' ' | sed 's/^ //')"
fi

mkdir -p "$PLUGIN_ROOT/sqldrivers"
cp "$built" "$PLUGIN_ROOT/sqldrivers/"

echo
echo "done. Put it on Qt's plugin path for anything that uses the mysql provider:"
echo "  export QT_PLUGIN_PATH=\"$PLUGIN_ROOT\""
