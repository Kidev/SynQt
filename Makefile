# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

# Developer commands for this checkout. Not the build.
#
# Three copies can shadow the checkout: an installed `~/.local/bin/synqt`, the vendored
# `tools/synqt/synqt/framework/`, and `site/`. `make doctor` reports them; `make clean-stale`
# removes them.

SHELL := /bin/bash
.DEFAULT_GOAL := help

# Outside the tree, so `make clean` leaves it.
DOCS_VENV ?= $(HOME)/.cache/synqt-docs-venv
DOCS_OUT  ?= $(HOME)/.cache/synqt-site
DOCS_PORT ?= 8000

# The host Qt kit. No default: the directory name differs per platform.
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
	@echo "    test-site      the built site's front page in a browser (after make docs)"
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

# An editable pipx install; --force replaces whatever `synqt` is on PATH.
.PHONY: cli
cli:
	@command -v pipx >/dev/null || { \
	    echo "pipx is not installed. It is what the guide installs SynQt with;"; \
	    echo "on Arch: pacman -S python-pipx"; exit 2; }
	pipx install --editable --force $(CLI)
	@echo
	@echo "synqt is now: $$(command -v synqt)"
	@synqt --help >/dev/null && echo "and it answers."

# The build backend decides what the vendored copy holds.
.PHONY: framework
framework:
	@$(PYTHON) -c "import sys; sys.path.insert(0, '$(CLI)'); \
	import _build_backend as b; b._vendor_framework(); \
	print('vendored framework refreshed from this checkout')"

$(DOCS_VENV):
	$(PYTHON) -m venv $(DOCS_VENV)

# One package per pip call, to bound disk use. Unquoted, so `-e <path>` is two arguments.
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

# The built site's front page, in a browser.
.PHONY: test-site
test-site:
	cd tests/site-home/verify && npm install --no-audit --no-fund && node verify.mjs $(DOCS_OUT)

# What CI runs.
.PHONY: test-cpp
test-cpp:
	@test -n "$(QT_HOST)" || { \
	    echo "QT_HOST is unset. Point it at a Qt 6.11 host kit, e.g."; \
	    echo "  make test-cpp QT_HOST=/opt/Qt/6.11.1/gcc_64"; exit 2; }
	QT_HOST=$(QT_HOST) tests/run-all.sh

.PHONY: lint
lint: lint-designrules lint-mermaid

# The editor's rules against the shared topologies.
.PHONY: lint-designrules
lint-designrules:
	node tools/check-designrules/check-designrules.mjs

# Every ```mermaid fence in docs/, parsed by mermaid. Installed in the check's own directory:
# npm at the root would prune ts-morph.
.PHONY: lint-mermaid
lint-mermaid:
	@test -d tools/check-mermaid/node_modules/mermaid || { \
	    echo "fetching the mermaid parser and a DOM for it..."; \
	    npm install --prefix tools/check-mermaid --no-save --no-fund --no-audit \
	        mermaid@11 jsdom; }
	node tools/check-mermaid/check-mermaid.mjs docs

# The node package the CLI's TypeScript backend needs.
.PHONY: node-deps
node-deps:
	@test -d node_modules/ts-morph || npm install --no-save --no-fund --no-audit ts-morph
	@node -e "require.resolve('ts-morph')" >/dev/null 2>&1 \
	    && echo "ts-morph is reachable; the TypeScript type backend can run" \
	    || { echo "ts-morph is still not reachable"; exit 1; }

.PHONY: check-all
check-all: test test-designer lint

# Docs

# --strict fails on a dead anchor. Built outside site/.
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

# Everything derived in this tree, every project's generated/ included.
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

# The copies that shadow this checkout. Both regenerate on demand.
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
