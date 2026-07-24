# pg_ladybug Makefile — PGXS, plain C
# Does NOT link liblbug at build time; it is dlopen()'d at runtime.

MODULES = pg_ladybug
EXTENSION = pg_ladybug
DATA = pg_ladybug--1.0.sql
OBJS = $(WIN32RES) pg_ladybug.o ladybug_bridge.o
PG_CPPFLAGS = -I.
SHLIB_LINK = -ldl
PG_CONFIG ?= pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Convenience test targets (local postgres, requires liblbug.so)
# -------------------------------------------------------------------
# Quick test: build, install, run test.sql against the default cluster
.PHONY: test test-install

test: install
	@echo "Running tests... (set ladybug.lib_path if liblbug is available)"
	psql -U postgres -v ON_ERROR_STOP=1 -f test.sql

test-install: install
	createdb -U postgres pg_ladybug_test 2>/dev/null || true
	psql -U postgres -d pg_ladybug_test -v ON_ERROR_STOP=1 -f test.sql

# Download liblbug shared library (requires gh CLI or curl)
.PHONY: download-liblbug
download-liblbug:
	@mkdir -p $(CURDIR)/lib
	LBUG_TARGET_DIR=$(CURDIR)/lib LBUG_LIB_KIND=shared bash scripts/download-liblbug.sh
	@echo "liblbug downloaded to $(CURDIR)/lib/"
	@ls -la $(CURDIR)/lib/
