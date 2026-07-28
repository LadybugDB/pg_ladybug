# pg_ladybug — Security & Correctness Review

Findings from a review of `pg_ladybug.c`, `ladybug_bridge.c`, and
`pg_ladybug--1.0.sql`. Issues 1–3 were reproduced against a live
PostgreSQL instance with the extension built and installed; each
reproduction is noted inline.

---

## Issue 1 — NULL arguments crash the backend (SEVERITY: high, DoS)

**Location:** `pg_ladybug--1.0.sql` (all four C functions declared
`CALLED ON NULL INPUT`); `pg_ladybug.c` (`ladybug_cypher`,
`ladybug_sql_query`, `ladybug_explain`, `ladybug_pushed_sql`).

All four SQL-callable C functions are declared `CALLED ON NULL INPUT`,
but each unconditionally does:

```c
text *t = PG_GETARG_TEXT_PP(0);
char  *s = text_to_cstring(t);   /* dereferences the datum */
```

With a NULL argument the datum is 0, so `text_to_cstring()` dereferences
a null/garbage pointer and the backend segfaults. Because a backend
crash takes the whole cluster into crash recovery, any unprivileged user
with EXECUTE permission on these functions can repeatedly crash the
entire database server.

**Reproduced:**

```sql
SELECT * FROM ladybug.sql_query(NULL) AS t(x int);
SELECT ladybug.explain(NULL);
SELECT ladybug.pushed_sql(NULL);
SELECT * FROM ladybug.cypher(NULL) AS t(x int);
```

All four produce: `server closed the connection unexpectedly ... the
server terminated abnormally`, followed by `FATAL: the database system
is in recovery mode` for subsequent connections.

**Suggested fix:** declare the functions `STRICT` in
`pg_ladybug--1.0.sql`, and additionally guard in C with
`PG_ARGISNULL(0)` (defense in depth: error or return NULL/empty set).

---

## Issue 2 — Results silently truncated to 64 rows (SEVERITY: high, data loss)

**Location:** `pg_ladybug.c` — `MAX_COLS` (64), `LadybugSRFContext`,
first-call row loops and per-call guard in `ladybug_cypher` and
`ladybug_sql_query`.

`MAX_COLS` is a *column* constant but is used as a cap on the number of
*rows* materialized:

```c
Datum *values[MAX_COLS];          /* actually indexed by row */
...
for (int i = 0; i < nrows && i < MAX_COLS; i++)   /* stores at most 64 rows */
...
if (srfctx->current_row < srfctx->nrows && srfctx->current_row < MAX_COLS)
```

`funcctx->max_calls` / `srfctx->nrows` are set to the true row count,
but only the first 64 rows are ever stored or returned — the rest are
silently dropped with no warning or error. This affects both
`ladybug.sql_query()` and `ladybug.cypher()` (pushdown path *and* the
`ladybug_bridge_execute_collect` fallback path).

**Reproduced:**

```sql
SELECT count(*) FROM
  ladybug.sql_query('SELECT generate_series(1,200) AS g') AS t(g int);
-- returns 64 (expected 200); min=1, max=64 — rows 65..200 silently lost
```

**Suggested fix:** remove the fixed cap entirely — allocate the row
array dynamically from the actual row count (e.g.
`HeapTuple *rows = palloc(nrows * sizeof(HeapTuple))`), store row count
and cursor in the SRF context, and drop the per-row single-element
`Datum` arrays and the unused `nulls` arrays. Never truncate silently;
if a cap is ever kept, it must raise an error.

---

## Issue 3 — No type check between result columns and the column
## definition list: crashes and silently wrong data (SEVERITY: high)

**Location:** `pg_ladybug.c` — column-mapping + `SPI_getbinval` loops in
`ladybug_cypher` and `ladybug_sql_query`.

Columns are matched by *name* (three fuzzy name-matching passes), and
the binary Datum from the SPI result column is then placed verbatim
into a tuple described by the caller's column definition list:

```c
vals[a] = SPI_getbinval(spi_tup, spi_tupdesc, spi_idx + 1, &isnull);
...
ht = heap_form_tuple(expected_tupdesc, vals, nls);
```

The actual column type (`spi_tupdesc`) and the declared type
(`expected_tupdesc`) are never compared. Consequences:

- **byval → byref mismatch** (e.g. result is `int`, declared `text`):
  the integer value is reinterpreted as a pointer → backend crash.
- **different-width byval mismatch** (e.g. result is `int8`, declared
  `int`): silently wrong values.

**Reproduced:**

```sql
SELECT * FROM ladybug.sql_query('SELECT 42 AS v') AS t(v text);
-- backend crash (connection lost)

SELECT * FROM ladybug.sql_query('SELECT 8589934592::int8 AS v') AS t(v int);
-- silently returns 0 (expected 8589934592 or an error)
```

**Suggested fix:** when a column is matched, compare
`TupleDescAttr(spi_tupdesc, spi_idx)->atttypid` against the expected
`atttypid`. On mismatch either (a) `ereport(ERROR)` naming the column
and both types, or (b) coerce safely via text: fetch with
`SPI_getvalue()` and convert with the expected type's input function
(`getTypeInputInfo` + `InputFunctionCall`), mirroring what the fallback
path in `ladybug_bridge.c` already does. Never reinterpret a binary
Datum across types. (The fuzzy name-matching passes 2/3 can also pick
the wrong column silently — e.g. expected `since` matching both
`a_since` and `b_since` picks the first — worth an ambiguity error or at
least a warning.)

---

## Issue 4 — Documented in-memory fallback skipped when connection init
## fails (SEVERITY: medium, availability)

**Location:** `ladybug_bridge.c` — `ladybug_bridge_acquire()`.

The GUC description and header comments promise: "If initialization at
this path fails, the extension falls back to in-memory mode." In fact
the fallback only covers `lbug_database_init()` failing. If the database
is created at the configured path but `lbug_connection_init()` fails,
the function returns NULL (hard ERROR for the caller) instead of falling
back to `:memory:` — an avoidable availability regression whenever the
on-disk catalog is usable enough to open but not to connect to.

**Suggested fix:** on `lbug_connection_init` failure in the storage
branch, `lbug_database_destroy()` the handle, record the error for the
WARNING, and fall through to the in-memory path like the
`lbug_database_init` failure case.

---

## Issue 5 — Changing `ladybug.pg_connstr` mid-session is silently
## ignored (SEVERITY: medium, wrong-database queries)

**Location:** `ladybug_bridge.c` — `LadybugBridge.attached` bool,
`ladybug_bridge_attach_postgres()`; `pg_ladybug.c` — `ensure_attached()`.

`ladybug.pg_connstr` is `PGC_USERSET`, but the bridge records only a
boolean "attached". Once ATTACH has succeeded in a backend, later
`SET ladybug.pg_connstr = '...'` changes are silently ignored: every
subsequent `ladybug.cypher()` / `pushed_sql()` / `explain()` call keeps
querying the originally attached database while the user believes they
switched. This yields wrong results with no indication of the cause.

**Suggested fix:** cache the connstr actually used for ATTACH (in
`TopMemoryContext`) in the bridge; on each attach, if the current GUC
differs from the cached one, `ereport(ERROR)` explaining the attachment
is fixed for the lifetime of the session (or implement DETACH +
re-ATTACH). Also see issue 6 about the "already" substring check, which
currently masks the engine's own duplicate-attach error.

---

## Issue 6 — `ladybug.pg_connstr` is `PGC_USERSET`: any user can make
## the server initiate arbitrary outbound connections (SEVERITY: medium, hardening)

**Location:** `pg_ladybug.c` — `_PG_init()` GUC definition.

Any unprivileged role can set `ladybug.pg_connstr` to an arbitrary
libpq connection string, which the embedded engine then dials *from
inside the PostgreSQL server process*. On hardened deployments where the
DB server has no general outbound network access, this hands every user
an SSRF-style primitive (arbitrary host/port connections, credential
probing). ATTACH error messages may also be echoed back via
`errdetail("%s", e)`, potentially reflecting connstr contents (including
passwords) into client-visible errors and server logs.

**Suggested fix:** define the GUC as `PGC_SUSET` (superuser-settable)
and avoid echoing raw engine errors that may contain the connstr into
error details; log sanitized messages instead.

---

## Issue 7 — "already"-substring error swallowing (SEVERITY: low, robustness)

**Location:** `ladybug_bridge.c` — `ladybug_bridge_attach_postgres()`.

Errors from the LOAD/ATTACH statements are ignored whenever the message
contains the substring `"already"`:

```c
if (e == NULL || strstr(e, "already") == NULL) { ...fail... }
/* otherwise: treated as success */
```

Any genuine failure whose text happens to contain "already" (e.g. a
catalog conflict such as "'pg' already exists with different
configuration") is treated as success, after which every Cypher query
runs against a wrong or half-configured catalog.

**Suggested fix:** rely on local bridge state (issue 5) rather than
message substrings, or match the engine's exact duplicate-object error
text.

---

## Minor / cosmetic

- `spi_execute_and_capture()` (`pg_ladybug.c`): accepts
  `SPI_OK_INSERT_RETURNING` / `SPI_OK_UPDATE_RETURNING` /
  `SPI_OK_DELETE_RETURNING`, but `SPI_execute` is called with
  `read_only = true`, so those statuses can never occur (dead code).
  The `expected_tupdesc` parameter is also unused.
- `LadybugSRFContext.nulls[]` arrays are allocated per row but always
  set to NULL and never read (cleanup folds into the issue-2 rework).
- `_PG_init()`: `snprintf` into the 1024-byte default-path buffer is not
  checked for truncation (a DataDir longer than ~1020 bytes silently
  truncates the default `ladybug.storage_path`).
- `ladybug_bridge_execute_collect()` /
  `ladybug_bridge_fill_tuplestore_from_query()`: an `InputFunctionCall`
  failure (malformed value for the declared type) longjmps out without
  destroying the in-flight `lbug_query_result` — a native-heap leak on
  the error path (PG memory is cleaned up by context reset). Consider
  `PG_TRY/PG_CATCH` cleanup if error paths become common.
- `ladybug_bridge_release()` is never called from anywhere; backend
  shutdown relies on process exit to release the engine's database and
  connection (acceptable, but consider an `on_proc_exit` callback).

---

## Verified OK during review

- `lbug_flat_tuple_get_value()` returns values with
  `_is_owned_by_cpp = true` (borrowed pointers into the reused flat
  tuple; confirmed by disassembly: `lbug_value_destroy` is a no-op for
  them). Not destroying them is correct, and all string copies are made
  before the next `lbug_query_result_get_next()`, which is required
  because the flat tuple is reused.
- The connstr embedded in `ATTACH '...'` is single-quote doubled — no
  SQL injection into the engine there.
- HeapTuples are formed before `SPI_finish()`, so no use-after-free of
  SPI tuple memory.
- `SRF_RETURN_NEXT` returns the HeapTuple Datum directly; the executor
  copies it, so allocation in the multi-call memory context is correct.
- `ladybug._graph_meta` receives default table privileges (no PUBLIC
  access), so label mappings are not world-writable.
