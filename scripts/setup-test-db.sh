#!/usr/bin/env bash
# pg_ladybug test database setup
#
# Creates the test database with node_* and rel_* tables following
# the convention that:
#   node_*  -> Cypher node labels
#   rel_*   -> Cypher relationship labels
#
# Usage:
#   ./scripts/setup-test-db.sh
#   ./scripts/setup-test-db.sh -p 5433 -d mydb

set -euo pipefail

PGPORT="${PGPORT:-5433}"
PGHOST="${PGHOST:-/var/run/postgresql}"
PGUSER="${PGUSER:-postgres}"
PGDATABASE="${PGDATABASE:-ladybug_test}"

while getopts "p:d:u:h:" opt; do
    case $opt in
        p) PGPORT="$OPTARG" ;;
        d) PGDATABASE="$OPTARG" ;;
        u) PGUSER="$OPTARG" ;;
        h) PGHOST="$OPTARG" ;;
        *) echo "Usage: $0 [-p port] [-d db] [-u user] [-h host]" >&2; exit 1 ;;
    esac
done

PSQL="psql -h $PGHOST -p $PGPORT -U $PGUSER"
DBCMD="$PSQL -d $PGDATABASE"

echo "=== Setting up test database ==="
echo "PG: $PGHOST:$PGPORT  DB: $PGDATABASE  User: $PGUSER"

# Create database if it doesn't exist
$PSQL -tc "SELECT 1 FROM pg_database WHERE datname = '$PGDATABASE'" | grep -q 1 \
    || $PSQL -c "CREATE DATABASE $PGDATABASE"

# Create extension
$DBCMD -c "CREATE EXTENSION IF NOT EXISTS pg_ladybug"

# Create node tables
$DBCMD <<'EOSQL'
-- Node tables
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

-- Relationship tables (fixture mirrors upstream sql_pushdown.test:
-- node_person Alice/35, Bob/25, Carol/45, Dave/28; node_city NYC, SF;
-- rel_knows Alice->Bob/2020, Bob->Carol/2021, Alice->Carol/2019,
-- Carol->Dave/2022; rel_likes Alice->Bob/5, Bob->Carol/8, Carol->Dave/3;
-- rel_livesin Alice->NYC, Bob->SF, Carol->NYC; FK constraints on
-- src_id/dst_id are required for pg_client rel_* auto-detection)
CREATE TABLE IF NOT EXISTS rel_knows (
    id SERIAL PRIMARY KEY,
    src_id INTEGER NOT NULL REFERENCES node_person(id),
    dst_id INTEGER NOT NULL REFERENCES node_person(id),
    since INTEGER DEFAULT 2020
);

CREATE TABLE IF NOT EXISTS rel_likes (
    id SERIAL PRIMARY KEY,
    src_id INTEGER NOT NULL REFERENCES node_person(id),
    dst_id INTEGER NOT NULL REFERENCES node_person(id),
    score INTEGER
);

CREATE TABLE IF NOT EXISTS rel_livesin (
    id SERIAL PRIMARY KEY,
    src_id INTEGER NOT NULL REFERENCES node_person(id),
    dst_id INTEGER NOT NULL REFERENCES node_city(id)
);

-- Sample data
-- Clear existing data and re-insert
TRUNCATE TABLE rel_livesin, rel_likes, rel_knows, node_city, node_person RESTART IDENTITY CASCADE;

INSERT INTO node_person (name, age) VALUES
    ('Alice', 35),
    ('Bob', 25),
    ('Carol', 45),
    ('Dave', 28);

INSERT INTO node_city (name, population) VALUES
    ('NYC', 8336000),
    ('SF', 874961);

INSERT INTO rel_knows (src_id, dst_id, since) VALUES
    (1, 2, 2020),
    (2, 3, 2021),
    (1, 3, 2019),
    (3, 4, 2022);

INSERT INTO rel_likes (src_id, dst_id, score) VALUES
    (1, 2, 5),
    (2, 3, 8),
    (3, 4, 3);

INSERT INTO rel_livesin (src_id, dst_id) VALUES
    (1, 1),
    (2, 2),
    (3, 1);
EOSQL

# Register labels in _graph_meta
$DBCMD <<'EOSQL'
SELECT ladybug.register_node('Person',  'node_person', 'id');
SELECT ladybug.register_node('City',    'node_city',   'id');
SELECT ladybug.register_edge('KNOWS',    'rel_knows',    'src_id', 'dst_id', 'id');
SELECT ladybug.register_edge('LIKES',    'rel_likes',    'src_id', 'dst_id', 'id');
SELECT ladybug.register_edge('LIVES_IN', 'rel_livesin', 'src_id', 'dst_id', 'id');
EOSQL

echo ""
echo "=== Test database ready ==="
echo ""
echo "Run tests:  ./scripts/test.sh"
echo "Connect:    psql -h $PGHOST -p $PGPORT -U $PGUSER -d $PGDATABASE"
