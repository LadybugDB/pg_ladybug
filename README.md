# pg_ladybug

**Cypher query support as a PostgreSQL extension.**

`pg_ladybug` embeds the [Ladybug](https://github.com/LadybugDB/ladybug) graph engine
via its C API (`liblbug`) and lets you run **Cypher** queries that read your **Postgres**
tables — with native Postgres execution under the hood.

## How it works

1. You declare which Postgres tables represent Cypher node/edge labels
   (`ladybug.register_node`, `ladybug.register_edge`).
2. You write a Cypher `MATCH` query.
3. `pg_ladybug` hands it to the embedded Ladybug planner via `dlopen("liblbug.so")`.
   Ladybug's `ForeignJoinPushDownOptimizer` detects that all tables are backed by
   the *same* Postgres database and rewrites the entire pattern into a **single SQL
   JOIN query**.
4. `pg_ladybug` extracts that SQL from the EXPLAIN plan (the exact technique from
   [Ladybug's DuckDB-attach notebook](https://github.com/LadybugDB/ladybug-icebug-notebook/blob/main/ladybug_duckdb_attached_starwars.ipynb))
   and runs it **natively via SPI** against the local Postgres tables.
5. Results stream back through a standard `SETOF record` SRF.

No second query engine runs in your Postgres backend. The only "foreign" engine is
`liblbug`, loaded *out-of-band* and used purely as a Cypher→SQL compiler.
DuckDB never loads in your Postgres server.

## Requirements

- **PostgreSQL 17** (other versions may work; tested on 17)
- **pg_config** (postgresql-server-dev package)
- **`liblbug.so`** at runtime (the Ladybug engine shared library; see Installation)
- **OpenSSL** (liblbug requires `libssl`/`libcrypto`)

## Installation

### Building the extension

```bash
make
sudo make install
```

### Getting liblbug

You need the Ladybug shared library available on your system. Download a prebuilt
release from the [Ladybug GitHub releases](https://github.com/LadybugDB/ladybug/releases):

```bash
# Download the latest release for your platform
curl -sL "https://github.com/LadybugDB/ladybug/releases/latest/download/liblbug-linux-x86_64.tar.gz" \
  | tar xz -C /usr/local/lib/

# Or use the vendored download script:
LBUG_LIB_KIND=shared LBUG_TARGET_DIR=/usr/local/lib bash scripts/download-liblbug.sh
```

Set the `ladybug.lib_path` GUC to point at the library before using Cypher functions:

```sql
SET ladybug.lib_path = '/usr/local/lib/liblbug.so';
```

### Ladybug postgres extension

To attach the current Postgres database to the Ladybug planner's catalog, the
Ladybug postgres extension must be installed in Ladybug's extension cache. This
happens automatically at runtime (`INSTALL postgres` + `LOAD postgres`) as long as
`liblbug` can reach the Ladybug extension repository.

If the automatic install fails, you can pre-install the extension by building
[ladybug-extension/postgres](https://github.com/LadybugDB/ladybug-extension/tree/main/postgres).

## Quick start

```sql
CREATE EXTENSION pg_ladybug;

-- Set the path to liblbug (do this once per session, or set in postgresql.conf)
SET ladybug.lib_path = '/usr/local/lib/liblbug.so';

-- Connection string for the Ladybug catalog (uses the current session by default)
SET ladybug.pg_connstr = 'host=/var/run/postgresql port=5432 dbname=mydb user=postgres';

-- Create a table and register it as a Cypher node label
CREATE TABLE persons (id SERIAL PRIMARY KEY, name TEXT, age INTEGER);
INSERT INTO persons VALUES (1, 'Alice', 30), (2, 'Bob', 25), (3, 'Carol', 35);

SELECT ladybug.register_node('Person', 'persons', 'id');

-- Run Cypher
SELECT * FROM ladybug.cypher(
  'MATCH (n:Person) RETURN n.name AS name, n.age AS age ORDER BY n.age'
) AS t(name text, age int);

-- Output:
--  name  | age
-- -------+-----
--  Bob   |  25
--  Alice |  30
--  Carol |  35

-- Or just see what SQL the planner would push down:
SELECT ladybug.pushed_sql('MATCH (n:Person) RETURN n.name, n.age ORDER BY n.age');

-- Result: SELECT name, age FROM persons ORDER BY age
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
| `ladybug.pg_connstr` | `""` | Connection string used to ATTACH this Postgres to Ladybug's catalog (auto-derived if empty) |

## How the bridge works

`ladybug_bridge.c` `dlopen`s `liblbug.so` at first use and resolves 19 C API
entry points via `dlsym` (declared in Ladybug's `c_api/lbug.h`). It creates an
in-memory Ladybug database, attaches the current Postgres as a foreign catalog
(`dbtype POSTGRES`), then runs `EXPLAIN <cypher>` for each call. The pushed-down
SQL is extracted from the plan text using the exact line-scanning algorithm from
[Ladybug's notebook](https://github.com/LadybugDB/ladybug-icebug-notebook/blob/main/ladybug_duckdb_attached_starwars.ipynb)
and executed via `SPI_execute`.

## License

PostgreSQL License — same as Ladybug and pg_ddate.
