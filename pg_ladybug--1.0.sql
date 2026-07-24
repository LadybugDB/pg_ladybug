-- pg_ladybug: Cypher query support through the embedded Ladybug engine.
-- Ladybug is dlopen()'d at runtime (set ladybug.lib_path); DuckDB is NOT
-- required inside the PostgreSQL backend.  The pushed-down SQL produced by
-- Ladybug's planner is executed natively by PostgreSQL via SPI.

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_ladybug" to load this file. \quit

-- Create the ladybug schema for catalog tables and helper functions
CREATE SCHEMA IF NOT EXISTS ladybug;

-- Graph metadata catalog: maps node/edge labels to tables
CREATE TABLE IF NOT EXISTS ladybug._graph_meta (
    id          SERIAL PRIMARY KEY,
    graph       TEXT NOT NULL DEFAULT 'main',
    kind        TEXT NOT NULL CHECK (kind IN ('node', 'edge')),
    label       TEXT NOT NULL,
    table_name  TEXT NOT NULL,
    id_column   TEXT NOT NULL DEFAULT 'id',
    props_json  TEXT,
    UNIQUE (graph, kind, label)
);

COMMENT ON TABLE ladybug._graph_meta IS
    'Maps Cypher node/edge labels to Postgres tables for the Ladybug planner';

-- -----------------------------------------------------------------------
-- ladybug.register_node(label, table_name, id_column, props_json, graph)
-- Register a Postgres table as a Cypher node label.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.register_node(
    label       TEXT,
    table_name  TEXT,
    id_column   TEXT DEFAULT 'id',
    props_json  TEXT DEFAULT NULL,
    graph       TEXT DEFAULT 'main'
)
RETURNS INTEGER
LANGUAGE SQL
AS $$
    INSERT INTO ladybug._graph_meta (graph, kind, label, table_name, id_column, props_json)
    VALUES (graph, 'node', label, table_name, id_column, props_json)
    ON CONFLICT (graph, kind, label) DO UPDATE SET
        table_name  = EXCLUDED.table_name,
        id_column   = EXCLUDED.id_column,
        props_json  = EXCLUDED.props_json
    RETURNING id;
$$;

-- -----------------------------------------------------------------------
-- ladybug.register_edge(label, table_name, from_col, to_col, id_column, graph)
-- Register a Postgres table as a Cypher edge label.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.register_edge(
    label       TEXT,
    table_name  TEXT,
    from_col    TEXT DEFAULT 'src_id',
    to_col      TEXT DEFAULT 'dst_id',
    id_column   TEXT DEFAULT 'id',
    graph       TEXT DEFAULT 'main'
)
RETURNS INTEGER
LANGUAGE SQL
AS $$
    INSERT INTO ladybug._graph_meta (graph, kind, label, table_name, id_column, props_json)
    VALUES (graph, 'edge', label, table_name, id_column,
            jsonb_build_object('from_col', from_col, 'to_col', to_col)::text)
    ON CONFLICT (graph, kind, label) DO UPDATE SET
        table_name  = EXCLUDED.table_name,
        id_column   = EXCLUDED.id_column,
        props_json  = EXCLUDED.props_json
    RETURNING id;
$$;

-- -----------------------------------------------------------------------
-- ladybug.cypher(cypher_text TEXT) -> SETOF record
-- Run a Cypher query: Ladybug's planner produces a pushed-down SQL string
-- which is executed natively by PostgreSQL via SPI.
-- Requires: ladybug.lib_path (GUC) pointing to liblbug, and
--           ladybug.pg_connstr (GUC) for the current Postgres.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.cypher(cypher_text TEXT)
RETURNS SETOF RECORD
LANGUAGE C
STABLE
CALLED ON NULL INPUT
PARALLEL UNSAFE
AS 'MODULE_PATHNAME', 'ladybug_cypher';

-- -----------------------------------------------------------------------
-- ladybug.sql_query(query TEXT) -> SETOF record
-- Run a raw SQL query natively via SPI (no Ladybug involvement).
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.sql_query(query TEXT)
RETURNS SETOF RECORD
LANGUAGE C
STABLE
CALLED ON NULL INPUT
PARALLEL UNSAFE
AS 'MODULE_PATHNAME', 'ladybug_sql_query';

-- -----------------------------------------------------------------------
-- ladybug.explain(cypher_text TEXT) -> TEXT
-- Return the raw Ladybug EXPLAIN plan text.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.explain(cypher_text TEXT)
RETURNS TEXT
LANGUAGE C
STABLE
CALLED ON NULL INPUT
PARALLEL UNSAFE
AS 'MODULE_PATHNAME', 'ladybug_explain';

-- -----------------------------------------------------------------------
-- ladybug.pushed_sql(cypher_text TEXT) -> TEXT
-- Return the pushed-down SQL extracted from the Ladybug EXPLAIN plan.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.pushed_sql(cypher_text TEXT)
RETURNS TEXT
LANGUAGE C
STABLE
CALLED ON NULL INPUT
PARALLEL UNSAFE
AS 'MODULE_PATHNAME', 'ladybug_pushed_sql';

-- -----------------------------------------------------------------------
-- ladybug.list_labels(graph TEXT DEFAULT 'main')
-- Show registered labels for a named graph.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.list_labels(
    graph TEXT DEFAULT 'main'
)
RETURNS TABLE(kind TEXT, label TEXT, table_name TEXT)
LANGUAGE SQL
STABLE
AS $$
    SELECT kind, label, table_name
    FROM ladybug._graph_meta
    WHERE graph = list_labels.graph
    ORDER BY kind, label;
$$;

-- -----------------------------------------------------------------------
-- ladybug.reset_graph(graph TEXT DEFAULT 'main')
-- Clear all mappings for a graph.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.reset_graph(
    graph TEXT DEFAULT 'main'
)
RETURNS INTEGER
LANGUAGE SQL
AS $$
    DELETE FROM ladybug._graph_meta WHERE graph = reset_graph.graph;
$$;