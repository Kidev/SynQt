# Contributing to SynQt

Thanks for helping build SynQt. This guide covers the license of your
contributions, the header every source file needs, and the code style.

## License of contributions and the CLA

SynQt's own code is licensed under Apache-2.0 (see [LICENSE](LICENSE)). To keep the project
free and open while preserving the ability to offer SynQt under other terms in the
future (for example a commercial or dual license), every contributor must agree to
the Contributor License Agreement in [CLA.md](CLA.md) before their contribution is merged.

In short, the CLA lets you keep ownership of your contribution while granting the
project a broad license, including the right to relicense your contribution as part
of SynQt under other terms. This is the same mechanism Qt itself and many
sustainable open source projects use. It does not change the fact that the public
SynQt releases are Apache-2.0.

How to accept: the project uses a CLA assistant (or a signed-off-by Developer
Certificate of Origin as configured on the repository). Follow the prompt on your
first pull request. Contributions cannot be merged without acceptance.

## Every source file needs an SPDX header

Add these two lines at the top of every source file you create, in the file's
comment syntax. This is the modern, machine-readable way to mark licensing and it
keeps license scanners happy.

C++, QML, JavaScript, and other `//` comment files:

```
// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0
```

CMake, shell, Python, YAML, and other `#` comment files:

```
# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0
```

Do not put SPDX headers or copyright lines into files that a developer's built
application will convey (for example generated client code): those artifacts are
governed by Qt's license, not SynQt's. See [docs/licensing.md](docs/licensing.md).

## Code style

The C++, QML, and JavaScript follow the official Qt conventions ([Qt Coding
Style](https://wiki.qt.io/Qt_Coding_Style), [Qt Coding
Conventions](https://wiki.qt.io/Coding_Conventions), [QML Coding
Conventions](https://doc.qt.io/qt-6/qml-codingconventions.html)), with three rules applied
everywhere, without exception:

1. **Always brace a control statement body**, even a single line one.
2. **Always use brace (uniform) initialization** (`int a{0}`), which also rejects
   narrowing.
3. **Never use a C-style cast. Every conversion is an explicit `static_cast<T>(x)`**
   (or `qobject_cast` for QObjects; `reinterpret_cast` or `const_cast` only at a C API
   boundary). This one departs from the Qt wiki, which permits the constructor form
   `int(myFloat)` for built in types: `T(x)` on a single expression is *defined* to be
   equivalent to `(T)x`, so it is a C-style cast in different syntax, with the same
   power to silently become a `reinterpret_cast` or strip `const`, and
   `-Wold-style-cast` does not see it. Use `static_cast` even where the conversion is a
   no op on your platform (`static_cast<qint64>(size())` is a real widening on a 32 bit
   target).

Also: no exceptions and no RTTI (`dynamic_cast`/`typeid`); `Q_OBJECT` in every QObject
subclass; `override` (not `virtual`) when reimplementing; keep lines under 100 columns.

Document a class or a member with a `///` block directly above its declaration, or a
member with a `///<` comment after it: those reach the
[generated C++ reference](https://synqt.org/api/), whose conventions and local build are
described in [docs/api-reference.md](docs/api-reference.md). A plain `//` comment stays a
note to the next reader of that line, which is the right choice for a remark about one
line of implementation.

- Follow the repository formatters (clang-format for C++, qmlformat for QML).
- Match the surrounding code and keep changes focused.

[docs/development.md](docs/development.md) maps the repository, names each runtime
library and what it is responsible for, and lists every test suite and how to run it.
Read it before your first change.

## Before you open a pull request

- Run `synqt check` (config and topology validation, contract and QML linting).
- Run the formatters yourself (clang-format, qmlformat); `synqt check` does not format.
- Run `synqt test`, including the transport spike test if you touched the transport
  (the QtRO over WebSockets path is the highest risk area).
- Describe what changed and why. Link the issue if there is one.

The CLA text and the licensing analysis are not legal advice. If you contribute on
behalf of an employer, make sure you have the right to do so.
