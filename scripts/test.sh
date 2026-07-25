#!/usr/bin/env bash
# pg_ladybug test runner
#
# Usage:
#   ./scripts/test.sh              # run tests against default PG cluster
#   ./scripts/test.sh -p 5433      # run tests against PG on port 5433
#   ./scripts/test.sh -d mydb      # use a specific database
#   ./scripts/test.sh -v           # verbose (log output from PG)
#
# Prerequisites:
#   - PostgreSQL cluster running (PG 18 recommended)
#   - Extension installed (make install)
#   - liblbug.so available (set ladybug.lib_path)
#   - Test database with node_*/rel_* tables (see ./scripts/setup-test-db.sh)

set -euo pipefail

PGPORT="${PGPORT:-5433}"
PGHOST="${PGHOST:-/var/run/postgresql}"
PGUSER="${PGUSER:-postgres}"
PGDATABASE="${PGDATABASE:-ladybug_test}"
VERBOSE=0

while getopts "p:d:u:h:v" opt; do
    case $opt in
        p) PGPORT="$OPTARG" ;;
        d) PGDATABASE="$OPTARG" ;;
        u) PGUSER="$OPTARG" ;;
        h) PGHOST="$OPTARG" ;;
        v) VERBOSE=1 ;;
        *) echo "Usage: $0 [-p port] [-d db] [-u user] [-h host] [-v]" >&2; exit 1 ;;
    esac
done

PSQL_CMD="psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE"
PG_ISREADY="pg_isready -h $PGHOST -p $PGPORT"

echo "=== pg_ladybug test runner ==="
echo "PG: $PGHOST:$PGPORT/$PGDATABASE as $PGUSER"
echo ""

# Check cluster is running
if ! $PG_ISREADY &>/dev/null; then
    echo "ERROR: PostgreSQL cluster not running on $PGHOST:$PGPORT"
    echo "Start it with: pg_ctlcluster 18 main start"
    exit 1
fi

# Check extension is installed
if ! $PSQL_CMD -c "SELECT 1 FROM pg_extension WHERE extname = 'pg_ladybug'" 2>/dev/null | grep -q 1; then
    echo "Creating extension pg_ladybug..."
    $PSQL_CMD -c "CREATE EXTENSION IF NOT EXISTS pg_ladybug"
fi

# Check liblbug availability
LIBLBUG_OK=$($PSQL_CMD -t -A -c "
    SELECT CASE WHEN length(ladybug.explain('RETURN 1')) > 0 THEN 'yes' ELSE 'no' END
" 2>/dev/null || echo "no")

echo "liblbug available: $LIBLBUG_OK"
echo ""

# Run the test suite
echo "=== Running tests ==="
if [ "$VERBOSE" -eq 1 ]; then
    $PSQL_CMD -c "SET client_min_messages TO LOG" -f test.sql
else
    $PSQL_CMD -f test.sql
fi

echo ""
echo "=== Done ==="
