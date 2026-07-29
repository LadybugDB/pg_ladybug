#!/usr/bin/env bash
# pg_ladybug test runner for PGXN CI (pgxn/pgxn-tools / pg-build-test)
#
# This script is discovered by pg-build-test and replaces the pgembed-based
# test.sh in scripts/.  It uses the PostgreSQL instance started by pg-start
# and runs the full test suite (pure SPI + Ladybug bridge via liblbug).

set -euo pipefail

PSQL="psql -U postgres"
PORT="${PGPORT:-5432}"
DB="ladybug_test"

echo "=== pg_ladybug CI test runner ==="
echo "PGPORT=$PORT  PGHOST=${PGHOST:-/var/run/postgresql}"
echo ""

# ------------------------------------------------------------------
# 1. Create the test database
# ------------------------------------------------------------------
echo "--- Creating test database ---"
$PSQL -tc "SELECT 1 FROM pg_database WHERE datname='$DB'" | grep -q 1 \
    || $PSQL -c "CREATE DATABASE $DB"
echo "OK"

# ------------------------------------------------------------------
# 2. Create extension (already installed by pg-build-test, but be safe)
# ------------------------------------------------------------------
echo "--- Installing extension ---"
$PSQL -d "$DB" -c "CREATE EXTENSION IF NOT EXISTS pg_ladybug"
echo "OK"

# ------------------------------------------------------------------
# 3. Create test tables (node_* / rel_* convention)
# ------------------------------------------------------------------
echo "--- Creating test tables ---"
$PSQL -d "$DB" <<'EOSQL'
CREATE TABLE IF NOT EXISTS node_person (
    id SERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    age INTEGER
);
CREATE TABLE IF NOT EXISTS node_city (
    id SERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    population INTEGER
);
CREATE TABLE IF NOT EXISTS rel_knows (
    id SERIAL PRIMARY KEY,
    src_id INTEGER NOT NULL,
    dst_id INTEGER NOT NULL,
    since INTEGER DEFAULT 2020
);
CREATE TABLE IF NOT EXISTS rel_lives_in (
    id SERIAL PRIMARY KEY,
    src_id INTEGER NOT NULL REFERENCES node_person(id),
    dst_id INTEGER NOT NULL REFERENCES node_city(id)
);

TRUNCATE TABLE rel_lives_in, rel_knows, node_city, node_person RESTART IDENTITY CASCADE;

INSERT INTO node_person (name, age) VALUES
    ('Alice', 30),
    ('Bob', 25),
    ('Carol', 35),
    ('Dave', 28);
INSERT INTO node_city (name, population) VALUES
    ('New York', 8336000),
    ('San Francisco', 874961),
    ('London', 8982000);
INSERT INTO rel_knows (src_id, dst_id, since) VALUES
    (1, 2, 2020),
    (1, 3, 2019),
    (2, 3, 2021),
    (3, 4, 2022);
INSERT INTO rel_lives_in (src_id, dst_id) VALUES
    (1, 1),
    (2, 2),
    (3, 3),
    (4, 2);
EOSQL
echo "OK"

# ------------------------------------------------------------------
# 4. Register labels in _graph_meta
# ------------------------------------------------------------------
echo "--- Registering labels ---"
$PSQL -d "$DB" <<'EOSQL'
SELECT ladybug.register_node('Person',  'node_person', 'id');
SELECT ladybug.register_node('City',    'node_city',   'id');
SELECT ladybug.register_edge('KNOWS',    'rel_knows',    'src_id', 'dst_id', 'id');
SELECT ladybug.register_edge('LIVES_IN', 'rel_lives_in', 'src_id', 'dst_id', 'id');
EOSQL
echo "OK"

# ------------------------------------------------------------------
# 5. Run the test suite (override default pgport in test.sql)
# ------------------------------------------------------------------
echo "--- Running test.sql ---"
$PSQL -d "$DB" -v ON_ERROR_STOP=1 -v pgport="$PORT" -f test.sql
echo ""

echo "=== ALL CI TESTS PASSED ==="
