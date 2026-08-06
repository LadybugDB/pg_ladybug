# pg_ladybug — Declarative Replication (Postgres → Ladybug)

## Goal

Let a user declaratively replicate **node** and **rel** tables from Postgres
into the embedded Ladybug graph engine, without writing the sync logic by
hand. This is the mirror image of `ladybug.cypher()`: reading pulls Postgres
data *into* the planner; replication pushes Postgres changes *out* to ladybug
native storage as Cypher `CREATE` / `MERGE` / `DELETE` statements.

## How it works (behind the scenes)

Replication is modelled on Postgres logical replication, split across the two
sides:

1. **Publisher = the local Postgres.** For each registered node/rel table we
   `CREATE PUBLICATION` with a conventional name (`ladybug_<graph>_pub`) that
   lists the tables, controlled by `ladybug.enable_replication(graph)`.
2. **Subscriber = the ladybug postgres client (pg_client).** The subscriber
   side is implemented as an `AFTER INSERT/UPDATE/DELETE` row trigger installed
   on each replicated table. Every change is captured, converted to a standard
   Cypher statement, and appended to `ladybug._replication_log`. That log is the
   change stream the ladybug client consumes.
3. **SQL → Cypher conversion.** `ladybug.replication_trigger()` is a C trigger
   function that turns a row change into standard Cypher:
   - `INSERT` → `CREATE`
   - `UPDATE` → `MERGE ... SET`
   - `DELETE` → `MATCH ... DELETE`
4. **Applying to ladybug native storage.** `ladybug.replay_replication(graph)`
   replays the captured change stream through the embedded Ladybug engine
   (each statement is executed via the bridge), re-materialising the graph.

Because the source tables stay the source of truth and the graph is rebuilt
from a log, the whole thing is declarative and idempotent-friendly.

## Generated Cypher (standard)

Given a node table registered as label `Person`, id column `id`:

```sql
-- INSERT of (id=1, name='Alice', age=30)
CREATE (n:Person {id: 1, name: 'Alice', age: 30})

-- UPDATE of that row to (name='Bob', age=31)
MERGE (n:Person {id: 1}) SET n.name = 'Bob', n.age = 31

-- DELETE of that row
MATCH (n:Person {id: 1}) DELETE n
```

For a rel table registered as edge `KNOWS` (from `src_id`, to `dst_id`):

```sql
-- INSERT src=1, dst=2, since=2020
MATCH (a {id: 1}), (b {id: 2}) CREATE (a)-[r:KNOWS {id: 1, since: 2020}]->(b)

-- UPDATE
MATCH (a {id: 1}), (b {id: 2}) MERGE (a)-[r:KNOWS {id: 1}]->(b) SET r.since = 2021

-- DELETE
MATCH ()-[r:KNOWS {id: 1}]->() DELETE r
```

Properties are emitted with:

- numeric / boolean columns bare (`30`, `true`),
- everything else as a single-quoted string (embedded quotes doubled).

## Declarative API

| Function | Purpose |
|---|---|
| `ladybug.enable_replication(graph, publish)` | Create the publication + triggers + catalog rows for a graph. Returns table count. Superuser only. |
| `ladybug.disable_replication(graph)` | Drop triggers + publication + catalog rows. Returns count. |
| `ladybug.replication_status(graph)` | Read-only table of the replication config. |
| `ladybug.replication_log(graph)` | Read-only table of the captured change stream. |
| `ladybug.replay_replication(graph)` | Apply the captured changes to the embedded ladybug engine. Returns number applied. |

## Catalog

`ladybug._replication` — one row per (graph, kind, label) that is being
replicated:

| column | meaning |
|---|---|
| `graph` | graph name (default `main`) |
| `kind` | `node` or `edge` |
| `label` | Cypher label |
| `table_name` | source Postgres table |
| `publication` | publication name (convention `ladybug_<graph>_pub`) |
| `trigger_name` | trigger installed on the table |
| `enabled` | whether replication is active |

`ladybug._replication_log` — append-only change stream:

| column | meaning |
|---|---|
| `id`, `ts` | sequence + timestamp |
| `graph`, `kind`, `label`, `table_name` | provenance |
| `operation` | `INSERT` / `UPDATE` / `DELETE` |
| `cypher` | the generated standard Cypher statement |

## Conventions

- Publication: `ladybug_<graph>_pub` (graph lowercased, non-alphanumerics → `_`).
- Trigger: `ladybug_repl_<label>` on each source table.
- Each replicated table must exist, be reachable via the search path, and have
  a primary key (the id column is used to build `MERGE` / `MATCH` / `DELETE`).

## Implementation notes

- `ladybug.replication_trigger()` is `LANGUAGE C` (`RETURNS trigger`): it reads
  trigger args `(graph, kind, label, id_column[, from_col, to_col])`, converts
  the `NEW`/`OLD` row via `SPI_getbinval` + output functions, and appends to the
  log via SPI. Its return value is ignored (AFTER trigger).
- `enable_replication` / `disable_replication` are `LANGUAGE plpgsql` and build
  the DDL (`CREATE PUBLICATION`, `CREATE TRIGGER`) plus catalog rows.
- `replay_replication` is `LANGUAGE C` and uses the existing bridge
  (`ladybug_bridge_direct_sql`) to run each captured statement against the
  embedded engine; statements that fail (e.g. the native node/rel table does
  not exist yet in ladybug) are skipped and reported, not re-raised.
- Logging works with **no** ladybug engine / liblbug dependency, so the
  declarative capture + conversion layer is fully testable in isolation.

## Flow diagram

```
 user                                    pg_ladybug                         embedded Ladybug
  │  enable_replication(graph)                 │                                  │
  ├───────────────────────────────────────────►│  ● CREATE PUBLICATION ladybug_*_pub
  │                                            │  ● CREATE TRIGGER ... ON each table
  │  INSERT/UPDATE/DELETE on node_/rel_ table  │                                  │
  ├───────────────────────────────────────────►│  replication_trigger()           │
  │                                            │    → build Cypher CREATE/MERGE/  │
  │                                            │            DELETE                 │
  │                                            │    → INSERT INTO _replication_log │
  │  replay_replication(graph)                 │                                  │
  ├───────────────────────────────────────────►│  ● read _replication_log          │
  │                                            │  ● run each statement ───────────►│  native graph data
```
