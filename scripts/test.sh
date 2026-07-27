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