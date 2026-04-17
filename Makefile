# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# The commands this checkout is worked on with, in one place.
#
# None of this is the build. `synqt build` builds a SynQt application and CMake builds the
# framework. This file is the developer's side of the desk. Install the CLI you are editing,
# run the suites, build the docs, and clear out the copies of SynQt that go stale and then
# quietly answer in place of the checkout.
#
# That last one is why this file exists. Three things on a developer's machine shadow the
# tree they are editing, and none of them announces itself:
#
#   * `~/.local/bin/synqt`: an installed CLI. A release binary sitting there answers
#     `synqt design` with whatever it was built from, months ago. `make cli` replaces it
#     with an editable install of this checkout.
#   * `tools/synqt/synqt/framework/`: a vendored copy of src/ and cmake/ that a wheel
#     build leaves behind. A stale one shadows `synqtc` in an interpreter that imports it,
#     and the whole test suite fails on contracts it parsed fine yesterday.
#   * `site/`: MkDocs output. `site/designer/` in particular is a copy of the editor, and
#     opening it instead of `synqt design` shows an editor from whenever it was last built.
#
# `make doctor` reports all three without changing anything; `make clean-stale` clears them.

SHELL := /bin/bash
.DEFAULT_GOAL := help

# Where the docs toolchain lives. A virtualenv rather than the system interpreter because
# the pinned mkdocs-material stack is large and is nobody else's business. Under $HOME/.cache
# rather than in the tree so `make clean` cannot take it out from under a docs build.
DOCS_VENV ?= $(HOME)/.cache/synqt-docs-venv
DOCS_OUT  ?= $(HOME)/.cache/synqt-site
DOCS_PORT ?= 8000

# The host Qt kit the framework and its suites compile against. Named for the host rather
# than for what it builds, so this is a question rather than a default. `gcc_64` is wrong on
# macOS and on Windows, and a default that is wrong is worse than one that is absent.
QT_HOST ?=

PYTHON ?= python3
CLI    := $(CURDIR)/tools/synqt

.PHONY: help
help:
	@echo "SynQt: make <target>"
	@echo
	@echo "  Set up"
	@echo "    cli            install the CLI from this checkout, editable (replaces any"
	@echo "                   release binary on PATH)"
	@echo "    framework      refresh the vendored src/ + cmake/ copy under tools/synqt"
	@echo "    docs-venv      create or update the docs toolchain in \$$DOCS_VENV"
	@echo "    node-deps      the node package the CLI's TypeScript backend needs"
	@echo
	@echo "  Check"
	@echo "    doctor         report which copy of SynQt answers, and what is stale"
	@echo "    test           the CLI and generator suites (pytest)"
	@echo "    test-designer  the design editor, driven in a real browser"
	@echo "    test-cpp       the framework and its C++ suites (needs QT_HOST=...)"
	@echo "    lint           the editor's rule parity and every mermaid fence in docs/"
	@echo "    check-all      test + test-designer + lint"
	@echo
	@echo "  Docs"
	@echo "    docs           build the site into \$$DOCS_OUT"
	@echo "    docs-serve     build it and serve it on http://127.0.0.1:$(DOCS_PORT)"
	@echo
	@echo "  Clean"
	@echo "    clean          build outputs, generated trees and caches in this tree"
	@echo "    clean-stale    the copies of SynQt that shadow this checkout"
	@echo "    distclean      clean + clean-stale + the docs venv and output"
	@echo
	@echo "  Variables: QT_HOST=$(if $(QT_HOST),$(QT_HOST),<unset>)  DOCS_VENV=$(DOCS_VENV)"

# Set up

# Editable, so the CLI on PATH is the tree being edited and not a snapshot of it.
#
# Through pipx rather than pip. This is an application and not a library, which is what pipx
# is for, and it is what the guide tells a reader to install SynQt with. It is also the only
# one of the two that works unaided on a distribution whose Python is externally managed
# (PEP 668). `pip install --user` is refused outright on Arch.
#
# `--force` because the point of this target is to take over from whatever is answering now,
# and the usual case is a release binary that pipx did not put there and will not replace on
# its own. pipx puts its entry point in ~/.local/bin, which is where that binary is, so after
# this the name resolves to the checkout.
.PHONY: cli
cli:
	@command -v pipx >/dev/null || { \
	    echo "pipx is not installed. It is what the guide installs SynQt with;"; \
	    echo "on Arch: pacman -S python-pipx"; exit 2; }
	pipx install --editable --force $(CLI)
	@echo
	@echo "synqt is now: $$(command -v synqt)"
	@synqt --help >/dev/null && echo "and it answers."

# The copy a wheel build vendors. Refreshed by asking the build backend to do it, rather
# than by copying the directories here. The backend decides what goes in, and a second
# opinion about that in a Makefile is the thing that would drift.
.PHONY: framework
framework:
	@$(PYTHON) -c "import sys; sys.path.insert(0, '$(CLI)'); \
	import _build_backend as b; b._vendor_framework(); \
	print('vendored framework refreshed from this checkout')"

$(DOCS_VENV):
	$(PYTHON) -m venv $(DOCS_VENV)

# One package per pip invocation, not the file in one go. The pinned mkdocs-material stack
# unpacks several large wheels at once, which has run a machine out of disk part way through
# and left the venv half built. One at a time is slower and finishes.
#
# Unquoted, so a line like `-e ./tools/pygments-synqt` reaches pip as the two
# arguments it is rather than as one nonsensical package name.
.PHONY: docs-venv
docs-venv: $(DOCS_VENV)
	@set -euo pipefail; \
	grep -v '^[[:space:]]*\(#\|$$\)' requirements.txt | while read -r line; do \
	    echo "  pip install $$line"; \
	    $(DOCS_VENV)/bin/pip install --quiet --disable-pip-version-check $$line; \
	done; \
	echo "docs toolchain ready in $(DOCS_VENV)"

# Check

.PHONY: doctor
doctor:
	@echo "== which synqt answers =="
	@found=$$(command -v synqt || true); \
	if [ -z "$$found" ]; then \
	    echo "  none on PATH. 'make cli' installs this checkout."; \
	elif head -c 4 "$$found" | grep -q ELF; then \
	    echo "  $$found"; \
	    echo "  a compiled release binary, built $$(date -r "$$found" '+%Y-%m-%d')."; \
	    echo "  It does NOT track this checkout. 'make cli' replaces it."; \
	else \
	    echo "  $$found"; \
	    echo "  a Python entry point; 'synqt --version' says what it resolves to."; \
	fi
	@echo
	@echo "== copies that shadow this checkout =="
	@if [ -d tools/synqt/synqt/framework ]; then \
	    echo "  tools/synqt/synqt/framework/  PRESENT (built $$(date -r tools/synqt/synqt/framework '+%Y-%m-%d'))"; \
	    if diff -rq tools/synqtc/synqtc tools/synqt/synqt/framework/tools/synqtc/synqtc \
	        --exclude=__pycache__ >/dev/null 2>&1; then \
	        echo "    and it matches tools/synqtc."; \
	    else \
	        echo "    and it DIFFERS from tools/synqtc: an interpreter that imports it"; \
	        echo "    parses contracts with the older compiler. 'make framework' refreshes it,"; \
	        echo "    'make clean-stale' removes it."; \
	    fi; \
	else \
	    echo "  tools/synqt/synqt/framework/  absent (good; a wheel build makes it)"; \
	fi
	@if [ -d site ]; then \
	    echo "  site/                         PRESENT (built $$(date -r site '+%Y-%m-%d'))"; \
	    echo "    MkDocs output. site/designer/ is an old copy of the editor;"; \
	    echo "    run 'synqt design' rather than opening it."; \
	else \
	    echo "  site/                         absent (good)"; \
	fi
	@echo
	@echo "== toolchain =="
	@printf "  %-10s %s\n" python "$$($(PYTHON) --version 2>&1)"
	@printf "  %-10s %s\n" node "$$(node --version 2>/dev/null || echo 'not installed')"
	@printf "  %-10s %s\n" doxygen "$$(doxygen --version 2>/dev/null || echo 'not installed')"
	@printf "  %-10s %s\n" mkdocs "$$($(DOCS_VENV)/bin/mkdocs --version 2>/dev/null || echo 'not installed; make docs-venv')"
	@printf "  %-10s %s\n" QT_HOST "$(if $(QT_HOST),$(QT_HOST),unset; needed by make test-cpp)"

.PHONY: test
test:
	cd $(CLI) && $(PYTHON) -m pytest -q

.PHONY: test-designer
test-designer:
	cd tests/designer && PYTHONPATH=$(CLI) node verify.mjs

# The whole host-kit story in one tree, which is what CI runs. The script wants the kit.
.PHONY: test-cpp
test-cpp:
	@test -n "$(QT_HOST)" || { \
	    echo "QT_HOST is unset. Point it at a Qt 6.11 host kit, e.g."; \
	    echo "  make test-cpp QT_HOST=/opt/Qt/6.11.1/gcc_64"; exit 2; }
	QT_HOST=$(QT_HOST) tests/run-all.sh

.PHONY: lint
lint: lint-designrules lint-mermaid

# The editor's rules against the shared topologies. Installs nothing. It imports the shipped
# asset by path and reads a JSON file beside it.
.PHONY: lint-designrules
lint-designrules:
	node tools/check-designrules/check-designrules.mjs

# Every ```mermaid fence in docs/, parsed by mermaid itself. mkdocs cannot catch a broken
# one: the theme parses diagrams in the reader's browser, so a bad fence builds green and
# renders as "Syntax error in text" on the live page.
#
# The parser is fetched on demand, the same floating major the docs theme loads at runtime.
# Into the check's own directory rather than the repository root, and this is not tidiness:
# there is no package.json at the root, so npm treats everything already in the root's
# node_modules as extraneous and prunes it. Installing there took `ts-morph` out and quietly
# turned five of the CLI's tests into skips. Node resolves an import from the importing
# file's own directory upward, so the check finds this copy first either way.
.PHONY: lint-mermaid
lint-mermaid:
	@test -d tools/check-mermaid/node_modules/mermaid || { \
	    echo "fetching the mermaid parser and a DOM for it..."; \
	    npm install --prefix tools/check-mermaid --no-save --no-fund --no-audit \
	        mermaid@11 jsdom; }
	node tools/check-mermaid/check-mermaid.mjs docs

# The one node package the CLI's TypeScript backend needs, installed the way its own error
# message asks for it. Without it that backend cannot run, and the tests covering it skip
# rather than fail, which is the quietest way for a capability to go missing.
.PHONY: node-deps
node-deps:
	@test -d node_modules/ts-morph || npm install --no-save --no-fund --no-audit ts-morph
	@node -e "require.resolve('ts-morph')" >/dev/null 2>&1 \
	    && echo "ts-morph is reachable; the TypeScript type backend can run" \
	    || { echo "ts-morph is still not reachable"; exit 1; }

.PHONY: check-all
check-all: test test-designer lint

# Docs

# --strict so a renamed heading fails here rather than shipping a dead anchor. Built into
# $(DOCS_OUT) rather than site/, because site/ in the tree is one of the stale copies this
# file exists to be rid of.
.PHONY: docs
docs:
	@test -x $(DOCS_VENV)/bin/mkdocs || { \
	    echo "no docs toolchain; run 'make docs-venv' first"; exit 2; }
	NO_MKDOCS_2_WARNING=true $(DOCS_VENV)/bin/mkdocs build --strict -d $(DOCS_OUT)
	@echo "built into $(DOCS_OUT)"

.PHONY: docs-serve
docs-serve: docs
	@echo "serving $(DOCS_OUT) on http://127.0.0.1:$(DOCS_PORT) (Ctrl-C to stop)"
	@cd $(DOCS_OUT) && $(PYTHON) -m http.server $(DOCS_PORT)

# Clean

# Everything derived, in this tree. `generated/` is per project, so every one of them goes:
# an example's is written from its synqt.yaml by the next build.
.PHONY: clean
clean:
	rm -rf build
	find . -path ./.git -prune -o -type d -name build -print0 | xargs -0 rm -rf
	find . -path ./.git -prune -o -type d -name generated -print0 | xargs -0 rm -rf
	find . -path ./.git -prune -o -type d -name __pycache__ -print0 | xargs -0 rm -rf
	find . -path ./.git -prune -o -type d -name '*.egg-info' -print0 | xargs -0 rm -rf
	rm -rf tools/synqt/dist
	@echo "cleaned build outputs, generated trees and caches"
	@echo "(node_modules is left alone: re-fetching it is a download, not a rebuild)"

# The copies that answer in place of this checkout. Both are regenerated on demand (the
# vendored tree by a wheel build, site/ by `mkdocs build`), so neither is work to lose.
.PHONY: clean-stale
clean-stale:
	rm -rf tools/synqt/synqt/framework site
	@echo "removed the vendored framework copy and the MkDocs output"
	@found=$$(command -v synqt || true); \
	if [ -n "$$found" ] && head -c 4 "$$found" | grep -q ELF; then \
	    echo; \
	    echo "note: $$found is still a compiled release binary and does not track"; \
	    echo "      this checkout. Run 'make cli' to replace it."; \
	fi

.PHONY: distclean
distclean: clean clean-stale
	rm -rf $(DOCS_VENV) $(DOCS_OUT)
	@echo "removed the docs toolchain and its output"
