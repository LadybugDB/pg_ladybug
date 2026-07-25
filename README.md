# pg_ladybug

**Cypher query support as a PostgreSQL extension.**

`pg_ladybug` embeds the [Ladybug](https://github.com/LadybugDB/ladybug) graph engine
via its C API (`liblbug`) and lets you run **Cypher** queries that read your **Postgres**
tables — with native Postgres execution under the hood.

## How it works

1. You declare which Postgres tables represent Cypher node/edge labels
   (`ladybug.register_node`, `ladybug.register_edge`), or use the `node_*`/`rel_*`
   naming convention.
2. You write a `MATCH` query in Cypher.
3. `pg_ladybug` hands it to the embedded Ladybug planner via `dlopen("liblbug.so")`.
   Ladybug's `ForeignJoinPushDownOptimizer` detects that all tables are backed by
   the *same* Postgres database and rewrites the entire pattern into a **single SQL
   JOIN query**.
4. `pg_ladybug` extracts that SQL from the EXPLAIN plan and runs it **natively via
   SPI** against your local Postgres tables.
5. Results stream back through a standard `SETOF record` SRF.

No second query engine runs in your Postgres backend. The only "foreign" engine is
`liblbug`, loaded *out-of-band* and used purely as a Cypher → SQL compiler.

## Requirements

- **PostgreSQL 18** (other versions may work; tested on 18)
- **pg_config** (postgresql-server-dev package)
- **`liblbug.so`** at runtime (the Ladybug engine shared library; see Installation)
- **OpenSSL** (liblbug requires `libssl`/`libcrypto`)

## Quick start

```bash
# Build and install the extension
make
sudo make install

# Start a test database
createdb -p 5433 ladybug_test
psql -p 5433 -d ladybug_test -c "CREATE EXTENSION pg_ladybug"

# Or use the setup script
bash scripts/setup-test-db.sh

# Run the tests
bash scripts/test.sh
```

## Manual test walkthrough

```sql
-- Connect to the test database
\c ladybug_test

-- Set liblbug path (adjust if liblbug is in a non-standard location)
SET ladybug.lib_path = 'liblbug.so';

-- Set connection string for the Ladybug ATTACH
SET ladybug.pg_connstr = 'host=/var/run/postgresql port=5433 dbname=ladybug_test user=postgres';

-- --- Simple node MATCH ---
-- Uses the node_person table (auto-detected by the postgres extension ATTACH)
SELECT * FROM ladybug.cypher(
  'MATCH (n:node_person) RETURN n.name, n.age ORDER BY n.age'
) AS t(name text, age int);

-- Output:
--  name  | age
-- -------+-----
--  Bob   |  25
--  Dave  |  28
--  Alice |  30
--  Carol |  35

-- --- Node MATCH with WHERE ---
SELECT * FROM ladybug.cypher(
  'MATCH (n:node_person) WHERE n.age > 28 RETURN n.name, n.age ORDER BY n.age'
) AS t(name text, age int);

-- Output:
--  name  | age
-- -------+-----
--  Alice |  30
--  Carol |  35

-- --- See the pushed SQL ---
SELECT ladybug.pushed_sql(
  'MATCH (n:node_person) WHERE n.age > 28 RETURN n.name, n.age ORDER BY n.age'
);
-- Returns: SELECT * FROM node_person WHERE age > 28 ORDER BY age

-- --- Relationship pattern (requires rel_* auto-detection in Ladybug extension) ---
-- When the Ladybug postgres extension has been modified to detect rel_* tables
-- as relationship tables, this will work:
SELECT * FROM ladybug.cypher(
  'MATCH (a:node_person)-[r:rel_knows]->(b:node_person) RETURN a.name, b.name, r.since'
) AS t(a_name text, b_name text, since int);
```

## Naming convention

Tables with the following prefixes are automatically recognized:

| Prefix   | Cypher role       | Example           |
|----------|-------------------|-------------------|
| `node_`  | Node label        | `node_person`     |
| `rel_`   | Relationship      | `rel_knows`       |

For `rel_*` tables, the source and destination node tables are determined by
foreign key constraints on `src_id` and `dst_id` columns.

You can also register tables manually without the naming convention using
`ladybug.register_node()` and `ladybug.register_edge()`.

## Test scripts

```bash
# Set up the test database with sample data
bash scripts/setup-test-db.sh

# Run the test suite
bash scripts/test.sh

# Options
bash scripts/test.sh -p 5433 -d ladybug_test -v
```

## SQL reference

| Function | Description |
|---|---|
| `ladybug.cypher(text)` → `SETOF record` | Translate Cypher → SQL via Ladybug planner, execute natively, return rows |
| `ladybug.sql_query(text)` → `SETOF record` | Run arbitrary SQL via SPI (pure Postgres, no liblbug needed) |
| `ladybug.explain(text)` → `text` | Return the Ladybug EXPLAIN plan as text |
| `ladybug.pushed_sql(text)` → `text` | Return the pushed-down SQL that `cypher()` would execute |
| `ladybug.register_node(label, table_name, id_column, props_json, graph)` → `int` | Map a Postgres table as a Cypher node label |
| `ladybug.register_edge(label, table_name, from_col, to_col, id_column, graph)` → `int` | Map a Postgres table as a Cypher edge label |
| `ladybug.list_labels(graph)` → `table` | List all registered node/edge labels for a graph |
| `ladybug.reset_graph(graph)` → `int` | Clear all label mappings for a graph |

## GUCs

| GUC | Default | Description |
|---|---|---|
| `ladybug.lib_path` | `liblbug.so` | Path to the Ladybug shared library (dlopen'd at runtime) |
| `ladybug.pg_connstr` | `""` | Connection string used to ATTACH this Postgres to Ladybug's catalog |

## Architecture

```
┌─────────────────────────────────────────────┐
│          PostgreSQL backend                  │
│                                              │
│  Cypher query → ladybug.cypher()             │
│       │                                      │
│       ▼                                      │
│  Ladybug planner (liblbug via dlopen)        │
│    - Parse Cypher                            │
│    - Optimize (ForeignJoinPushDown)          │
│    - Produce EXPLAIN plan                    │
│       │                                      │
│       ▼                                      │
│  extract_pushed_sql()                        │
│    - Parse plan text                         │
│    - Extract "Function:" section (SQL)       │
│    - OR construct SELECT from plan nodes     │
│       │                                      │
│       ▼                                      │
│  SPI_execute(pushed_sql)                     │
│    - Native Postgres execution               │
│    - Return rows via SRF                     │
│                                              │
└─────────────────────────────────────────────┘
```

## License

PostgreSQL License.
