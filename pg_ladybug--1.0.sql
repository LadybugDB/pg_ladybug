-- pg_ladybug: Cypher query support through the embedded Ladybug engine.
-- Ladybug is linked at build time (-llbug); the pushed-down SQL produced by
-- Ladybug's planner is executed natively by PostgreSQL via SPI.

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_ladybug" to load this file. \quit

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
-- Requires: ladybug.pg_connstr (GUC) for the current Postgres.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.cypher(cypher_text TEXT)
RETURNS SETOF RECORD
LANGUAGE C
STABLE
STRICT
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
STRICT
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
STRICT
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
STRICT
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
    SELECT 1::int;
$$;

-- =======================================================================
-- Declarative replication (Postgres -> Ladybug)
--
-- Replication of node/rel tables from Postgres into the embedded Ladybug
-- engine.  The publisher side is a conventional Postgres PUBLICATION
-- (`ladybug_<graph>_pub`); the subscriber side is an AFTER row trigger that
-- converts each SQL change into a standard Cypher CREATE/MERGE/DELETE
-- statement and records it in ladybug._replication_log (the change stream
-- the ladybug postgres client consumes).  ladybug.replay_replication()
-- applies that stream to ladybug native storage.
-- =======================================================================

-- Replication configuration: one row per replicated (graph, kind, label).
CREATE TABLE IF NOT EXISTS ladybug._replication (
    graph        TEXT NOT NULL DEFAULT 'main',
    kind         TEXT NOT NULL CHECK (kind IN ('node', 'edge')),
    label        TEXT NOT NULL,
    table_name   TEXT NOT NULL,
    publication  TEXT NOT NULL,
    trigger_name TEXT NOT NULL,
    enabled      BOOLEAN NOT NULL DEFAULT true,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (graph, kind, label)
);

COMMENT ON TABLE ladybug._replication IS
    'Declarative replication config: which node/rel tables are replicated '
    'from Postgres into the Ladybug graph engine';

-- Append-only change stream: the SQL->Cypher statements captured by the
-- replication triggers.
CREATE TABLE IF NOT EXISTS ladybug._replication_log (
    id         BIGSERIAL PRIMARY KEY,
    ts         TIMESTAMPTZ NOT NULL DEFAULT now(),
    graph      TEXT NOT NULL,
    kind       TEXT NOT NULL,
    label      TEXT NOT NULL,
    table_name TEXT NOT NULL,
    operation  TEXT NOT NULL CHECK (operation IN ('INSERT', 'UPDATE', 'DELETE')),
    cypher     TEXT NOT NULL
);

COMMENT ON TABLE ladybug._replication_log IS
    'Change stream of SQL->Cypher statements captured by replication triggers';

-- -----------------------------------------------------------------------
-- ladybug.enable_replication(graph, publish)
-- Declaratively start replicating a graph's registered node/rel tables
-- from Postgres into Ladybug.  Creates a conventional PUBLICATION and an
-- AFTER INSERT/UPDATE/DELETE row trigger on each table (the trigger emits
-- standard Cypher CREATE/MERGE/DELETE statements into _replication_log).
-- Superuser only (required by CREATE PUBLICATION).  Returns the number of
-- tables enabled.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.enable_replication(
    gr       TEXT DEFAULT 'main',
    publish  TEXT DEFAULT 'INSERT,UPDATE,DELETE'
)
RETURNS INTEGER
LANGUAGE plpgsql
VOLATILE
AS $$
DECLARE
    v_pub    TEXT := 'ladybug_' ||
                     regexp_replace(lower(gr), '[^a-z0-9_]+', '_', 'g') || '_pub';
    v_tables TEXT[];
    v_count  INTEGER := 0;
    r         RECORD;
    v_has_pk  BOOLEAN;
    v_trg    TEXT;
    v_from   TEXT;
    v_to     TEXT;
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = current_user AND rolsuper) THEN
        RAISE EXCEPTION 'ladybug: enable_replication requires superuser privileges';
    END IF;

    IF EXISTS (SELECT 1 FROM ladybug._replication WHERE graph = gr) THEN
        RAISE EXCEPTION 'ladybug: replication already enabled for graph "%"', gr;
    END IF;

    -- Collect the registered tables and validate them.
    FOR r IN
        SELECT kind, label, table_name, id_column, props_json
        FROM ladybug._graph_meta
        WHERE graph = gr
        ORDER BY kind, label
    LOOP
        IF to_regclass(r.table_name) IS NULL THEN
            RAISE EXCEPTION 'ladybug: table "%" for label "%" does not exist',
                            r.table_name, r.label;
        END IF;

        SELECT EXISTS (
            SELECT 1 FROM pg_constraint con
            WHERE con.conrelid = to_regclass(r.table_name) AND con.contype = 'p'
        ) INTO v_has_pk;
        IF NOT v_has_pk THEN
            RAISE EXCEPTION 'ladybug: table "%" has no PRIMARY KEY (needed as the '
                            'replication key)', r.table_name;
        END IF;

        v_tables := v_tables || r.table_name;
        v_count  := v_count + 1;
    END LOOP;

    IF v_count = 0 THEN
        RAISE EXCEPTION 'ladybug: no registered node/rel tables for graph "%"', gr;
    END IF;

    -- Publisher side: a conventional publication listing the tables.
    EXECUTE 'CREATE PUBLICATION ' || v_pub || ' FOR TABLE ' ||
            (SELECT string_agg(quote_ident(t), ', ')
               FROM unnest(v_tables) AS t) ||
            ' WITH (publish = ' || quote_literal(publish) || ')';

    -- Subscriber side: an AFTER row trigger per table that emits the
    -- SQL->Cypher statement into _replication_log.
    FOR r IN
        SELECT kind, label, table_name, id_column, props_json
        FROM ladybug._graph_meta
        WHERE graph = gr
        ORDER BY kind, label
    LOOP
        v_trg := 'ladybug_repl_' || r.label;
        IF r.kind = 'edge' THEN
            SELECT (p->>'from_col'), (p->>'to_col')
              INTO v_from, v_to
              FROM (SELECT r.props_json::jsonb AS p) x;
            EXECUTE format(
                'CREATE TRIGGER %I AFTER INSERT OR UPDATE OR DELETE ON %I '
                'FOR EACH ROW EXECUTE FUNCTION '
                'ladybug.replication_trigger(%L, %L, %L, %L, %L, %L)',
                v_trg, r.table_name, gr, r.kind, r.label, r.id_column,
                v_from, v_to);
        ELSE
            EXECUTE format(
                'CREATE TRIGGER %I AFTER INSERT OR UPDATE OR DELETE ON %I '
                'FOR EACH ROW EXECUTE FUNCTION '
                'ladybug.replication_trigger(%L, %L, %L, %L)',
                v_trg, r.table_name, gr, r.kind, r.label, r.id_column);
        END IF;

        INSERT INTO ladybug._replication
            (graph, kind, label, table_name, publication, trigger_name)
        VALUES (gr, r.kind, r.label, r.table_name, v_pub, v_trg);
    END LOOP;

    RETURN v_count;
END;
$$;

-- -----------------------------------------------------------------------
-- ladybug.disable_replication(graph)
-- Stop replicating a graph: drop the triggers, drop the publication, and
-- remove the catalog rows.  Returns the number of tables disabled.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.disable_replication(
    gr TEXT DEFAULT 'main'
)
RETURNS INTEGER
LANGUAGE plpgsql
VOLATILE
AS $$
DECLARE
    v_count INTEGER := 0;
    v_pub   TEXT;
    r       RECORD;
BEGIN
    FOR r IN
        SELECT table_name, trigger_name, publication
        FROM ladybug._replication
        WHERE graph = gr
    LOOP
        BEGIN
            EXECUTE format('DROP TRIGGER IF EXISTS %I ON %I',
                           r.trigger_name, r.table_name);
        EXCEPTION WHEN OTHERS THEN
            RAISE NOTICE 'ladybug: could not drop trigger % on %',
                         r.trigger_name, r.table_name;
        END;
        v_pub   := r.publication;
        v_count := v_count + 1;
    END LOOP;

    IF v_pub IS NOT NULL THEN
        BEGIN
            EXECUTE 'DROP PUBLICATION IF EXISTS ' || v_pub;
        EXCEPTION WHEN OTHERS THEN
            RAISE NOTICE 'ladybug: could not drop publication %', v_pub;
        END;
    END IF;

    DELETE FROM ladybug._replication WHERE graph = gr;
    RETURN v_count;
END;
$$;

-- -----------------------------------------------------------------------
-- ladybug.replication_status(graph)
-- Read-only view of the replication config for a graph.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.replication_status(
    gr TEXT DEFAULT 'main'
)
RETURNS TABLE(kind TEXT, label TEXT, table_name TEXT,
              publication TEXT, trigger_name TEXT, enabled BOOLEAN)
LANGUAGE SQL
STABLE
AS $$
    SELECT kind, label, table_name, publication, trigger_name, enabled
    FROM ladybug._replication
    WHERE graph = gr
    ORDER BY kind, label;
$$;

-- -----------------------------------------------------------------------
-- ladybug.replication_log(graph)
-- Read-only view of the captured change stream for a graph.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.replication_log(
    gr TEXT DEFAULT 'main'
)
RETURNS TABLE(id BIGINT, ts TIMESTAMPTZ, kind TEXT, label TEXT,
              table_name TEXT, operation TEXT, cypher TEXT)
LANGUAGE SQL
STABLE
AS $$
    SELECT id, ts, kind, label, table_name, operation, cypher
    FROM ladybug._replication_log
    WHERE graph = gr
    ORDER BY id;
$$;

-- -----------------------------------------------------------------------
-- ladybug.replication_trigger() -> trigger
-- AFTER INSERT/UPDATE/DELETE row trigger installed by enable_replication.
-- Converts the changed row into a standard Cypher CREATE/MERGE/DELETE
-- statement and records it in ladybug._replication_log.
-- Trigger args: (graph, kind, label, id_column[, from_col, to_col]).
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.replication_trigger()
RETURNS TRIGGER
LANGUAGE C
AS 'MODULE_PATHNAME', 'ladybug_replication_trigger';

-- -----------------------------------------------------------------------
-- ladybug.replay_replication(graph)
-- Replay the captured change stream as Cypher statements into the embedded
-- Ladybug engine (the apply side of the subscription).  Returns the number
-- of statements successfully applied.  Statements that fail (e.g. missing
-- native node/rel table in ladybug) are skipped and reported as NOTICEs.
-- -----------------------------------------------------------------------
CREATE OR REPLACE FUNCTION ladybug.replay_replication(
    gr TEXT DEFAULT 'main'
)
RETURNS INTEGER
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
AS 'MODULE_PATHNAME', 'ladybug_replay_replication';