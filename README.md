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
3. `pg_ladybug` hands it to the embedded Ladybug planner. Ladybug's planner
   detects that all tables are backed by the *same* Postgres database and
   rewrites the entire pattern into a **single SQL JOIN query**.
4. `pg_ladybug` extracts that SQL from the EXPLAIN plan and runs it **natively via
   SPI** against your local Postgres tables.
5. Results stream back through a standard `SETOF record` SRF.

No second query engine runs in your Postgres backend. `liblbug` is linked at
build time and used purely as a Cypher → SQL compiler.

## Requirements

- **PostgreSQL 17+** (tested on 17 and 18)
- **pg_config** (postgresql-server-dev package)
- **`liblbug.so` v0.19.0+** (Ladybug engine shared library; see Installation)
- **`libpg_client.lbug_extension`** (pg_client extension for ATTACH; see Installation)
- **OpenSSL** (liblbug requires `libssl`/`libcrypto`)

## Installation

### 1. Download liblbug and pg_client

You need **liblbug v0.19.0 or later** and the **pg_client extension**.

```bash
# Download liblbug v0.19.0 (shared library)
# Set LBUG_VERSION or use a nightly build RUN_ID
export LBUG_PRECOMPILED_RUN_ID=<run_id>
export LBUG_TARGET_DIR=./lib
export LBUG_LIB_KIND=shared
bash scripts/download-liblbug.sh

# Download pg_client extension (nightly build from LadybugDB/extensions)
# Place libpg_client.lbug_extension next to liblbug.so
cp /path/to/libpg_client.lbug_extension lib/
```

The `lib/` directory should contain:
```
lib/
├── liblbug.so -> liblbug.so.0
├── liblbug.so.0 -> liblbug.so.0.19.0.*
├── liblbug.so.0.19.0.*
└── libpg_client.lbug_extension
```

### 2. Build and install the extension

```bash
make
sudo make install
```

### 3. Start a test database

```bash
bash scripts/setup-test-db.sh
```

Or manually:

```sql
CREATE EXTENSION pg_ladybug;
```

## Quick start

```bash
# Run the full test suite (uses pgembed — no pre-installed PG needed)
bash scripts/test.sh

# Or use an existing PG cluster
bash scripts/setup-test-db.sh
bash scripts/test.sh
```

## Manual test walkthrough

```sql
-- Connect to the test database
\c ladybug_test

-- Set connection string for the Ladybug ATTACH (via pg_client)
SET ladybug.pg_connstr = 'host=/var/run/postgresql port=5433 dbname=ladybug_test user=postgres';

-- --- Simple node MATCH ---
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

-- --- Node MATCH with ORDER BY ---
SELECT * FROM ladybug.cypher(
  'MATCH (n:node_person) RETURN n.name, n.age ORDER BY n.age'
) AS t(name text, age int);

-- --- See the pushed SQL ---
SELECT ladybug.pushed_sql(
  'MATCH (n:node_person) RETURN n.name, n.age ORDER BY n.age'
);
-- Returns: SELECT * FROM node_person ORDER BY age

-- --- Relationship pattern (requires rel_* tables with FK constraints) ---
SELECT * FROM ladybug.cypher(
  'MATCH (a:node_person)-[r:rel_knows]->(b:node_person) RETURN a.name, b.name, r.since'
) AS t(a_name text, b_name text, since int);
```

**Note on labels vs table names:** The pg_client extension registers tables
using their raw names. Use `node_person` (the table name) as the label
in Cypher queries. Label→table mapping via `ladybug.register_node()` is
used by the SQL extraction layer.

## Naming convention

Tables with the following prefixes are automatically recognized by the
pg_client extension:

| Prefix | Cypher role  | Example       |
|--------|--------------|---------------|
| `node_`| Node label   | `node_person` |
| `rel_` | Relationship | `rel_knows`   |

For `rel_*` tables, the source and destination node tables are determined by
foreign key constraints on `src_id` and `dst_id` columns.

You can also register tables manually using `ladybug.register_node()` and
`ladybug.register_edge()`.

## Test scripts

```bash
# Run the test suite (uses pgembed — no pre-installed PG needed)
bash scripts/test.sh

# With verbose output
bash scripts/test.sh -v
```

The test suite uses [pgembed](https://pypi.org/project/pgembed/) to start a
temporary PostgreSQL instance, build the extension against it, install
pg_ladybug + pg_client, and run the full test suite.

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
| `ladybug.pg_connstr` | `""` | Connection string used to ATTACH this Postgres to Ladybug's catalog via pg_client |

## Architecture

```
┌─────────────────────────────────────────────┐
│          PostgreSQL backend                  │
│                                              │
│  Cypher query → ladybug.cypher()             │
│       │                                      │
│       ▼                                      │
│  Ladybug planner (liblbug, compile-time      │
│  linked via -llbug)                          │
│    - Parse Cypher                            │
│    - Discover tables via pg_client           │
│      (libpq → information_schema)            │
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

The bridge uses the `pg_client` Ladybug extension (loaded from
`libpg_client.lbug_extension`) to ATTACH the local PostgreSQL database.
pg_client queries `information_schema.tables` and `information_schema.columns`
via libpq, making it compatible with all PostgreSQL versions.

## Updating liblbug

```bash
# Download a specific nightly build
export LBUG_PRECOMPILED_RUN_ID=<run_id>
bash scripts/download-liblbug.sh

# Or download the latest release
bash scripts/download-liblbug.sh
```

## License

PostgreSQL License.
