#!/usr/bin/env bash
# pg_ladybug test runner
#
# Uses pgembed to start an embedded PostgreSQL instance, builds the
# extension against it, installs pg_ladybug + pg_client extension,
# and runs the test suite.
#
# Usage:
#   ./scripts/test.sh                    # run with default uv
#   ./scripts/test.sh -v                 # verbose output

set -euo pipefail

VERBOSE=0
while getopts "v" opt; do
    case $opt in
        v) VERBOSE=1 ;;
        *) echo "Usage: $0 [-v]" >&2; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# On macOS the pgembed-bundled pg_config has a stale -isysroot baked in after
# an Xcode upgrade (it points at a deleted SDK), which breaks the build with
# 'stdio.h not found'. Repair it by selecting homebrew clang and a valid SDK.
# Only applied when the user hasn't already set CC / PG_SYSROOT, so explicit
# values always win.
if [ "$(uname -s)" = "Darwin" ]; then
    if [ -z "${CC:-}" ]; then
        for c in /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang; do
            if [ -x "$c" ]; then export CC="$c"; break; fi
        done
    fi
    if [ -z "${PG_SYSROOT:-}" ]; then
        sdk_base="/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs"
        # Prefer the rolling MacOSX.sdk; fall back to any versioned one.
        if [ -d "$sdk_base/MacOSX.sdk" ]; then
            export PG_SYSROOT="$sdk_base/MacOSX.sdk"
        else
            for sdk in "$sdk_base"/MacOSX*.sdk; do
                if [ -d "$sdk" ]; then export PG_SYSROOT="$sdk"; break; fi
            done
        fi
    fi
fi

echo "=== pg_ladybug test runner ==="
echo ""

# Run the test with pgembed fixture
if [ "$VERBOSE" -eq 1 ]; then
    uv run --with pgembed --with psycopg[binary] python3 "$SCRIPT_DIR/test_with_pgembed.py" 2>&1
else
    uv run --with pgembed --with psycopg[binary] python3 "$SCRIPT_DIR/test_with_pgembed.py" 2>&1
fi

echo ""
echo "=== Done ==="