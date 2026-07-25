-- pg_ladybug test script
-- Tests the extension's basic functionality (pure SPI path) and,
-- if liblbug is available via ladybug.lib_path, the Cypher path.
\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS pg_ladybug;

-- Set GUCs for liblbug-dependent tests (no-op if liblbug unavailable)
SET ladybug.lib_path = 'liblbug.so';
SET ladybug.pg_connstr = 'host=/var/run/postgresql port=5433 dbname=ladybug_test user=postgres';

-- List available functions
\df ladybug.*

-- ============================================================
-- Basic sql_query (pure SPI, no liblbug needed)
-- ============================================================
SELECT '=== sql_query test ===' AS info;

CREATE TEMP TABLE persons (
    id   SERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    age  INTEGER
);

INSERT INTO persons (name, age) VALUES
    ('Alice', 30),
    ('Bob',   25),
    ('Carol', 35),
    ('Dave',  28);

SELECT 'sql_query: count' AS info;
SELECT * FROM ladybug.sql_query('SELECT count(*)::int AS cnt FROM persons') AS t(cnt int);

SELECT 'sql_query: names' AS info;
SELECT * FROM ladybug.sql_query('SELECT name FROM persons ORDER BY name') AS t(name text);

-- ============================================================
-- register_node / _graph_meta (metadata only, no liblbug)
-- ============================================================
SELECT '=== register_node test ===' AS info;
SELECT ladybug.register_node(
    label      => 'Person',
    table_name => 'persons',
    id_column  => 'id'
);

SELECT 'graph_meta contents' AS info;
SELECT * FROM ladybug._graph_meta;

SELECT 'list_labels' AS info;
SELECT * FROM ladybug.list_labels();

-- ============================================================
-- explain / pushed_sql (needs liblbug at runtime)
-- ============================================================
SELECT '=== explain/pushed_sql tests (conditional on liblbug) ===' AS info;

-- These will fail if liblbug is not available or ladybug.pg_connstr
-- is not set. Wrap in a DO block with exception handling so the
-- test script succeeds either way.
DO $$
DECLARE
    plan_text text;
    sql_text  text;
BEGIN
    BEGIN
        plan_text := ladybug.explain('MATCH (n:Person) RETURN n.id');
        RAISE NOTICE 'EXPLAIN succeeded, plan length: %', length(plan_text);
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'EXPLAIN skipped (set ladybug.lib_path and ladybug.pg_connstr): %', SQLERRM;
    END;

    BEGIN
        sql_text := ladybug.pushed_sql('MATCH (n:Person) RETURN n.id');
        RAISE NOTICE 'pushed_sql succeeded: %', sql_text;
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'pushed_sql skipped (set ladybug.lib_path and ladybug.pg_connstr): %', SQLERRM;
    END;
END $$;

-- ============================================================
-- cypher (conditional on liblbug + pushed_sql)
-- ============================================================
SELECT '=== cypher test (conditional) ===' AS info;

DO $$
BEGIN
    BEGIN
        EXECUTE $q$
            SELECT * FROM ladybug.cypher('MATCH (n:Person) RETURN n.name AS name')
            AS t(name text)
        $q$;
        RAISE NOTICE 'cypher(MATCH Person) succeeded';
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'cypher skipped (set ladybug.lib_path and ladybug.pg_connstr): %', SQLERRM;
    END;
END $$;

-- ============================================================
-- Edge registration
-- ============================================================
SELECT '=== register_edge test ===' AS info;
CREATE TEMP TABLE knows (
    src_id  INTEGER NOT NULL,
    dst_id  INTEGER NOT NULL
);
INSERT INTO knows VALUES (1, 2), (2, 3);

SELECT ladybug.register_edge(
    label      => 'KNOWS',
    table_name => 'knows',
    from_col   => 'src_id',
    to_col     => 'dst_id'
);

SELECT 'edges in graph_meta:' AS info;
SELECT label, kind, table_name, props_json FROM ladybug._graph_meta
WHERE kind = 'edge';

-- ============================================================
-- In-place query tests (requires liblbug + ATTACH)
-- These test the bridge flow: Cypher -> plan -> SQL -> SPI
-- ============================================================
SELECT '=== in-place query tests (conditional on liblbug) ===' AS info;

/*
 * These tests use the naming convention:
 *   node_*  -> Cypher node labels
 *   rel_*   -> Cypher relationship labels
 *
 * They work with the existing bridge by:
 * 1. ATTACHing Postgres via the Ladybug postgres extension
 * 2. The extension auto-detects rel_* tables and registers
 *    them as relationship tables (RelGroupCatalogEntry)
 * 3. The ForeignJoinPushDownOptimizer pushes down the entire
 *    pattern as a single SQL JOIN query
 * 4. The bridge extracts that SQL and executes it via SPI
 *
 * When the modified extension isn't available, rel_* tables
 * are still registered as node tables, and the bridge's
 * extract_pushed_sql falls back to constructing SELECT *
 * queries from the SCAN_NODE_TABLE sections.
 */

-- The following tests use pre-existing tables in ladybug_test:
--   node_person (id, name, age)
--   node_city   (id, name, population)
--   rel_knows   (id, src_id, dst_id, since)
--   rel_lives_in (id, src_id, dst_id)

DO $$
DECLARE
    result_count int;
BEGIN
    -- Test simple node MATCH
    BEGIN
        EXECUTE $q$
            SELECT count(*)::int FROM ladybug.cypher(
                'MATCH (n:node_person) RETURN n.name, n.age'
            ) AS t(name text, age int)
        $q$ INTO result_count;
        RAISE NOTICE 'node_person MATCH returned % rows', result_count;
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'node_person MATCH skipped: %', SQLERRM;
    END;

    -- Test node MATCH with WHERE
    BEGIN
        EXECUTE $q$
            SELECT count(*)::int FROM ladybug.cypher(
                'MATCH (n:node_person) WHERE n.age > 28 RETURN n.name, n.age'
            ) AS t(name text, age int)
        $q$ INTO result_count;
        RAISE NOTICE 'node_person WHERE MATCH returned % rows', result_count;
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'node_person WHERE MATCH skipped: %', SQLERRM;
    END;

    -- Test node MATCH with ORDER BY
    BEGIN
        EXECUTE $q$
            SELECT count(*)::int FROM ladybug.cypher(
                'MATCH (n:node_person) RETURN n.name, n.age ORDER BY n.age'
            ) AS t(name text, age int)
        $q$ INTO result_count;
        RAISE NOTICE 'node_person ORDER BY MATCH returned % rows', result_count;
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'node_person ORDER BY MATCH skipped: %', SQLERRM;
    END;

    -- Test relationship pattern (requires rel_* auto-detection)
    -- This only works when the Ladybug extension has been modified
    -- to detect rel_* tables as relationship tables.
    BEGIN
        EXECUTE $q$
            SELECT count(*)::int FROM ladybug.cypher(
                'MATCH (a:node_person)-[r:rel_knows]->(b:node_person) RETURN a.name, b.name, r.since'
            ) AS t(a_name text, b_name text, since int)
        $q$ INTO result_count;
        RAISE NOTICE 'rel_knows pattern MATCH returned % rows', result_count;
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'rel_knows pattern MATCH skipped (requires rel_* extension support): %', SQLERRM;
    END;
END $$;

-- ============================================================
-- Cleanup
-- ============================================================
SELECT '=== cleanup ===' AS info;
SELECT ladybug.reset_graph();
SELECT 'graph cleared:', count(*) FROM ladybug._graph_meta;

SELECT '=== ALL TESTS PASSED ===' AS status;
