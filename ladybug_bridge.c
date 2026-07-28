/*
 * ladybug_bridge.c
 *
 * Bridge to the embedded Ladybug graph engine (liblbug) via compile-time
 * linking against liblbug.so.
 *
 * pg_ladybug links directly to liblbug at build time (no dlopen).  The
 * bridge creates a Ladybug database + connection at the path given by
 * the ladybug.storage_path GUC (defaults to <DataDir>/storage.lbdb), and
 * uses the planner to translate Cypher into a pushed-down SQL string.
 * If the GUC is empty or initialization at the configured path fails,
 * the bridge falls back to an in-memory database and emits a WARNING.
 *
 * The pushed-down SQL is then executed natively in the PostgreSQL backend
 * via SPI by pg_ladybug.c.  Ladybug is used purely as a Cypher->SQL
 * compiler; the storage is used for its catalog (table/schema metadata),
 * not for graph data inside the PG backend.
 */

#include "postgres.h"

#include <string.h>
#include "utils/memutils.h"
#include <stdlib.h>
#include <dlfcn.h>

#include "lib/lbug.h"
#include "utils/guc.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"
#include "access/htup_details.h"
#include "fmgr.h"

/* ================================================================ */
/*  LadybugBridge: cached database + connection                      */
/* ================================================================ */

typedef struct LadybugBridge
{
    lbug_database      database;       /* in-memory Ladybug DB   */
    lbug_connection    connection;     /* live connection        */
    bool               inited;         /* database+conn created  */
    bool               extension_loaded;/* pg_client extension loaded */
    bool               attached;       /* ATTACH done this backend */
    char              *attached_connstr;/* connstr used for ATTACH, in TopMemoryContext */
} LadybugBridge;

/* One bridge per backend (cached after first successful init). */
static LadybugBridge bridge = {0};

/* ================================================================ */
/*  Forward declarations of public API                               */
/* ================================================================ */

LadybugBridge *ladybug_bridge_acquire(const char **err_msg);
char  *ladybug_bridge_direct_sql(LadybugBridge *b, const char *sql, const char **err_msg);
bool   ladybug_bridge_attach_postgres(LadybugBridge *b, const char *pg_connstr, const char **err_msg);
char  *ladybug_bridge_pushed_sql(LadybugBridge *b, const char *cypher, const char **err_msg);
char  *ladybug_bridge_explain(LadybugBridge *b, const char *cypher, const char **err_msg);
char  *ladybug_bridge_execute_query(LadybugBridge *b, const char *query, const char **err_msg);
int    ladybug_bridge_fill_tuplestore_from_query(LadybugBridge *b, const char *query, Tuplestorestate *ts, TupleDesc tupdesc, const char **err_msg);
int    ladybug_bridge_execute_collect(LadybugBridge *b, const char *query, TupleDesc tupdesc, HeapTuple **out_tuples, const char **err_msg);
void   ladybug_bridge_release(LadybugBridge *b);

/* ================================================================ */
/*  Internal helpers                                                 */
/* ================================================================ */

/* ================================================================ */
/*  Public bridge API (used by pg_ladybug.c)                         */
/* ================================================================ */

/*
 * ladybug_bridge_acquire -- create a Ladybug database + connection.
 * Tries the path given by the ladybug.storage_path GUC first; falls back
 * to an in-memory database only if the GUC is empty or initialization at
 * the configured path fails.  Returns a pointer to the static bridge, or
 * NULL on failure (with *err_msg set).  Cached after first success.
 */
LadybugBridge *
ladybug_bridge_acquire(const char **err_msg)
{
    lbug_system_config cfg;
    lbug_state st;
    const char *storage_path_guc;
    char       *storage_path = NULL;
    bool        storage_attempted = false;
    char       *storage_err = NULL;

    if (bridge.inited)
        return &bridge;

    cfg = lbug_default_system_config();

    /*
     * Try the configured storage path first.  The GUC may be empty (user
     * explicitly disabled persistent storage) or unset (defaults to
     * <DataDir>/storage.lbdb, computed by pg_ladybug.c).
     */
    storage_path_guc = GetConfigOptionByName("ladybug.storage_path", NULL, false);
    if (storage_path_guc != NULL && storage_path_guc[0] != '\0')
    {
        storage_path = pstrdup(storage_path_guc);
        storage_attempted = true;

        st = lbug_database_init(storage_path, cfg, &bridge.database);
        if (st == 0)
        {
            /* Storage init succeeded; now create the connection. */
            st = lbug_connection_init(&bridge.database, &bridge.connection);
            if (st != 0)
            {
                /*
                 * Connection init failed even though database init succeeded.
                 * Record the error, clean up, and fall through to the
                 * in-memory fallback below (consistent with the documented
                 * fallback-to-in-memory contract).
                 */
                if (storage_err == NULL)
                    storage_err = psprintf("lbug_connection_init failed (state=%d) for storage '%s'",
                                           (int)st, storage_path);
                lbug_database_destroy(&bridge.database);
                pfree(storage_path);
                storage_path = NULL;
                memset(&bridge, 0, sizeof(bridge));
                /* Fall through to in-memory fallback below. */
            }
            else
            {
                bridge.inited = true;
                bridge.attached = false;
                bridge.extension_loaded = false;
                pfree(storage_path);
                elog(LOG, "pg_ladybug: ladybug engine initialized with persistent storage");
                return &bridge;
            }
        }
        else
        {
            if (storage_err == NULL)
                storage_err = psprintf("lbug_database_init failed (state=%d) for storage '%s'",
                                       (int)st, storage_path);
            pfree(storage_path);
            storage_path = NULL;
            /* Fall through to in-memory fallback below. */
        }
    }

    /* Fall back to in-memory mode. */
    st = lbug_database_init(":memory:", cfg, &bridge.database);
    if (st != 0)
    {
        if (err_msg)
        {
            if (storage_attempted && storage_err != NULL)
                *err_msg = psprintf("ladybug: %s; in-memory fallback also failed (state=%d)",
                                    storage_err, (int)st);
            else
                *err_msg = psprintf("ladybug: lbug_database_init failed (state=%d) for :memory:",
                                    (int)st);
        }
        if (storage_err) pfree(storage_err);
        memset(&bridge, 0, sizeof(bridge));
        return NULL;
    }

    st = lbug_connection_init(&bridge.database, &bridge.connection);
    if (st != 0)
    {
        if (err_msg)
        {
            if (storage_attempted && storage_err != NULL)
                *err_msg = psprintf("ladybug: %s; in-memory connection init also failed (state=%d)",
                                    storage_err, (int)st);
            else
                *err_msg = psprintf("ladybug: lbug_connection_init failed (state=%d) for :memory:",
                                    (int)st);
        }
        lbug_database_destroy(&bridge.database);
        if (storage_err) pfree(storage_err);
        memset(&bridge, 0, sizeof(bridge));
        return NULL;
    }

    bridge.inited = true;
    bridge.attached = false;

    if (storage_attempted && storage_err != NULL)
    {
        ereport(WARNING,
                (errmsg("pg_ladybug: failed to initialize ladybug storage, "
                        "falling back to in-memory mode"),
                 errdetail("%s", storage_err)));
        pfree(storage_err);
    }
    else
    {
        elog(LOG, "pg_ladybug: ladybug engine using in-memory storage "
                  "(persistent storage disabled or not configured)");
    }
    return &bridge;
}

/*
 * Run an arbitrary query string through the Ladybug connection and return
 * lbug_query_result_to_string() (the plan text for EXPLAIN, or a result
 * string for other queries).  The returned string is a palloc'd copy;
 * the original is freed via lbug_destroy_string.
 *
 * On failure returns NULL and sets *err_msg (palloc'd by us).
 */
char *
ladybug_bridge_direct_sql(LadybugBridge *b, const char *sql, const char **err_msg)
{
    lbug_query_result result;
    lbug_state st;
    char *raw;
    char *copy;
    char *lbug_err;

    if (b == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: bridge not initialized");
        return NULL;
    }

    memset(&result, 0, sizeof(result));

    st = lbug_connection_query(&b->connection, sql, &result);
    if (st != 0 || !lbug_query_result_is_success(&result))
    {
        lbug_err = lbug_query_result_get_error_message(&result);
        if (err_msg)
            *err_msg = psprintf("ladybug: query failed: %s",
                                lbug_err ? lbug_err : "(no error message)");
        if (lbug_err)
            lbug_destroy_string(lbug_err);
        lbug_query_result_destroy(&result);
        return NULL;
    }

    raw = lbug_query_result_to_string(&result);
    copy = NULL;
    if (raw)
    {
        copy = pstrdup(raw);
        lbug_destroy_string(raw);
    }
    lbug_query_result_destroy(&result);
    return copy;
}

/*
 * Issue the INSTALL/LOAD/ATTACH sequence so the planner can resolve
 * local Postgres tables.  Idempotent (guarded by bridge.attached).
 * Returns true on success.
 */

/*
 * Issue the INSTALL/LOAD/ATTACH sequence so the planner can resolve
 * local Postgres tables.  Idempotent (guarded by bridge.attached).
 * Returns true on success.
 *
 * Errors mentioning "already" (e.g. extension already installed) are
 * treated as non-fatal.
 */
bool
ladybug_bridge_attach_postgres(LadybugBridge *b, const char *pg_connstr, const char **err_msg)
{
    const char *e;

    if (b == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: bridge not initialized");
        return false;
    }
    if (b->attached)
    {
        /*
         * Already attached.  If the caller passes a different connstr,
         * reject the change (see issue 5).  The bridge is bound to the
         * original connstr for the lifetime of the backend.
         */
        if (pg_connstr && b->attached_connstr &&
            strcmp(pg_connstr, b->attached_connstr) != 0)
        {
            if (err_msg)
                *err_msg = psprintf(
                    "ladybug: cannot change ladybug.pg_connstr from "
                    "\"%s\" to \"%s\" within the same session; "
                    "ATTACH is fixed for the lifetime of the backend",
                    b->attached_connstr, pg_connstr);
            return false;
        }
        return true;
    }

    e = NULL;

    /*
     * LOAD the pg_client extension, unless we already did it in this
     * bridge session (tracked by extension_loaded).
     */
    if (!b->extension_loaded)
    {
        Dl_info dl_info;
        char *ext_path = NULL;

        if (dladdr((void *)lbug_default_system_config, &dl_info) && dl_info.dli_fname)
        {
            /* dl_info.dli_fname is the full path to liblbug.so */
            char *lib_dir = pstrdup(dl_info.dli_fname);
            char *slash = strrchr(lib_dir, '/');
            if (slash)
            {
                *slash = '\0';
                ext_path = psprintf("%s/libpg_client.lbug_extension", lib_dir);
            }
            pfree(lib_dir);
        }

        if (ext_path)
        {
            StringInfoData load_sql;
            initStringInfo(&load_sql);
            appendStringInfoString(&load_sql, "LOAD '");
            appendStringInfoString(&load_sql, ext_path);
            appendStringInfoString(&load_sql, "';");

            if (!ladybug_bridge_direct_sql(b, load_sql.data, &e))
            {
                /*
                 * If the extension was already loaded (the persistent
                 * database may have it from a previous session), treat
                 * the "already exists" error as non-fatal but still
                 * check for a more specific error.
                 */
                if (e == NULL || strstr(e, "already exists") == NULL)
                {
                    if (err_msg) *err_msg = e;
                    else if (e) pfree((char *)e);
                    pfree(load_sql.data);
                    pfree(ext_path);
                    return false;
                }
                if (e) { pfree((char *)e); e = NULL; }
            }
            pfree(load_sql.data);
            pfree(ext_path);
        }
        else
        {
            if (err_msg) *err_msg = pstrdup("ladybug: cannot locate liblbug.so path");
            return false;
        }

        b->extension_loaded = true;
    }

    /* ATTACH '<connstr>' AS pg (dbtype PG_CLIENT); */
    if (!b->attached)
    {
        StringInfoData attach_sql;
        initStringInfo(&attach_sql);
        appendStringInfoString(&attach_sql, "ATTACH '");
        for (const char *p = pg_connstr; *p; p++)
        {
            if (*p == '\'')
                appendStringInfoString(&attach_sql, "''");
            else
                appendStringInfoChar(&attach_sql, *p);
        }
        appendStringInfoString(&attach_sql, "' AS pg (dbtype PG_CLIENT);");

        if (!ladybug_bridge_direct_sql(b, attach_sql.data, &e))
        {
            /*
             * If the ATTACH was already done (persistent storage from a
             * previous session), treat the "already exists" error as
             * non-fatal.
             *
             * For genuine failures, use a sanitized message rather than
             * echoing the raw engine error (which may contain the connstr
             * including passwords).
             */
            if (e == NULL || strstr(e, "already exists") == NULL)
            {
                if (err_msg)
                    *err_msg = pstrdup(
                        "ladybug: ATTACH Postgres failed; check connection "
                        "settings and server logs for details");
                if (e) pfree((char *)e);
                pfree(attach_sql.data);
                return false;
            }
            if (e) { pfree((char *)e); e = NULL; }
        }
        pfree(attach_sql.data);

        /*
         * Save the connstr used for ATTACH in TopMemoryContext so it
         * persists across transactions for the lifetime of the backend.
         */
        if (b->attached_connstr)
            pfree(b->attached_connstr);
        b->attached_connstr = MemoryContextStrdup(TopMemoryContext, pg_connstr);
        b->attached = true;
    }

    return true;
}

/*
 * Run EXPLAIN <cypher> through Ladybug and extract the pushed-down SQL.
 * Uses the new C API function lbug_connection_get_pushed_sql() which walks
 * the logical plan directly, rather than parsing the textual EXPLAIN output.
 * Returns a palloc'd SQL string, or NULL if no pushdown was possible
 * (with *err_msg set to a descriptive message).
 */
char *
ladybug_bridge_pushed_sql(LadybugBridge *b, const char *cypher, const char **err_msg)
{
    char *sql;
    lbug_state st;

    if (b == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: bridge not initialized");
        return NULL;
    }

    st = lbug_connection_get_pushed_sql(&b->connection, cypher, &sql);
    if (st != LbugSuccess || sql == NULL)
    {
        char *lbug_err;

        lbug_err = lbug_get_last_error();
        if (err_msg)
        {
            if (lbug_err)
                *err_msg = psprintf("ladybug: could not extract pushed-down SQL: %s", lbug_err);
            else
                *err_msg = pstrdup("ladybug: could not extract pushed-down SQL "
                                    "(no pushdown operator found in plan). "
                                    "Use ladybug.explain() for the full plan.");
        }
        if (lbug_err) lbug_destroy_string(lbug_err);
        return NULL;
    }

    /* sql is now an lbug-allocated string; copy it to palloc'd memory */
    {
        char *copy = pstrdup(sql);
        lbug_destroy_string(sql);
        return copy;
    }
}

/*
 * Run EXPLAIN <cypher> and return the full plan text (palloc'd).
 * Exposes the raw Ladybug plan for debugging.
 */
char *
ladybug_bridge_explain(LadybugBridge *b, const char *cypher, const char **err_msg)
{
    StringInfoData explain_sql;
    const char *e;
    char *plan_text;

    if (b == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: bridge not initialized");
        return NULL;
    }

    initStringInfo(&explain_sql);
    appendStringInfoString(&explain_sql, "EXPLAIN ");
    appendStringInfoString(&explain_sql, cypher);

    e = NULL;
    plan_text = ladybug_bridge_direct_sql(b, explain_sql.data, &e);
    pfree(explain_sql.data);

    if (plan_text == NULL)
    {
        if (err_msg) *err_msg = e;
        else if (e) pfree((char *)e);
        return NULL;
    }

    return plan_text;
}

/*
 * ladybug_bridge_execute_query - run a Cypher/SQL query and return
 * the result as a string (like ladybug_bridge_direct_sql but
 * returns the raw result without the "explain result" prefix).
 *
 * On failure returns NULL and sets *err_msg.
 */
char *
ladybug_bridge_execute_query(LadybugBridge *b, const char *query, const char **err_msg)
{
    if (b == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: bridge not initialized");
        return NULL;
    }

    return ladybug_bridge_direct_sql(b, query, err_msg);
}

/*
 * ladybug_bridge_fill_tuplestore_from_query - run a query through the
 * Ladybug connection and fill a tuplestore with the results.
 * Returns the number of rows filled, or -1 on error.
 *
 * Each row value is converted to a PG Datum via InputFunctionCall.
 */
int
ladybug_bridge_fill_tuplestore_from_query(LadybugBridge *b,
    const char *query,
    Tuplestorestate *ts,
    TupleDesc tupdesc,
    const char **err_msg)
{
    lbug_query_result result;
    lbug_state st;
    char *lbug_err;
    int num_cols;
    int natts;
    int nrows;

    if (b == NULL || !b->inited)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: bridge not initialized");
        return -1;
    }

    memset(&result, 0, sizeof(result));

    st = lbug_connection_query(&b->connection, query, &result);
    if (st != 0 || !lbug_query_result_is_success(&result))
    {
        lbug_err = lbug_query_result_get_error_message(&result);
        if (err_msg)
            *err_msg = psprintf("ladybug: query failed: %s",
                                lbug_err ? lbug_err : "(no error message)");
        if (lbug_err)
            lbug_destroy_string(lbug_err);
        lbug_query_result_destroy(&result);
        return -1;
    }

    num_cols = (int)lbug_query_result_get_num_columns(&result);
    natts = tupdesc->natts;

    if (num_cols != natts)
    {
        if (err_msg)
            *err_msg = psprintf("ladybug: column count mismatch: query returns %d columns, expected %d",
                                num_cols, natts);
        lbug_query_result_destroy(&result);
        return -1;
    }

    nrows = 0;
    while (lbug_query_result_has_next(&result))
    {
        lbug_flat_tuple flat_tuple;
        Datum *values;
        bool  *nulls;
        HeapTuple tuple;

        memset(&flat_tuple, 0, sizeof(flat_tuple));

        st = lbug_query_result_get_next(&result, &flat_tuple);
        if (st != 0)
        {
            if (err_msg)
                *err_msg = pstrdup("ladybug: error fetching next tuple");
            lbug_query_result_destroy(&result);
            return -1;
        }

        values = (Datum *) palloc0(natts * sizeof(Datum));
        nulls  = (bool  *) palloc0(natts * sizeof(bool));

        for (int a = 0; a < natts; a++)
        {
            lbug_value val;
            char *str;

            memset(&val, 0, sizeof(val));

            st = lbug_flat_tuple_get_value(&flat_tuple, (uint64_t)a, &val);
            if (st != 0 || lbug_value_is_null(&val))
            {
                nulls[a] = true;
                values[a] = (Datum) 0;
                continue;
            }

            str = NULL;
            st = lbug_value_get_string(&val, &str);
            if (st != 0 || str == NULL)
                str = lbug_value_to_string(&val);

            if (str != NULL)
            {
                Oid typid, typioparam, typinput;
                int32 typmod;
                FmgrInfo finfo;

                typid = TupleDescAttr(tupdesc, a)->atttypid;
                typmod = TupleDescAttr(tupdesc, a)->atttypmod;
                getTypeInputInfo(typid, &typinput, &typioparam);
                fmgr_info(typinput, &finfo);

                values[a] = InputFunctionCall(&finfo, str, typioparam, typmod);
                nulls[a] = false;

                lbug_destroy_string(str);
            }
            else
            {
                nulls[a] = true;
                values[a] = (Datum) 0;
            }
        }

        tuple = heap_form_tuple(tupdesc, values, nulls);
        tuplestore_puttuple(ts, tuple);
        heap_freetuple(tuple);

        pfree(values);
        pfree(nulls);
        nrows++;
    }

    lbug_query_result_destroy(&result);
    return nrows;
}

/*
 * ladybug_bridge_execute_collect - run a query through the Ladybug
 * connection and return the result rows as a palloc'd array of
 * HeapTuples, allocated in the current memory context.  Each
 * HeapTuple conforms to the caller's TupleDesc.
 *
 * Returns the number of tuples (>= 0) on success, or -1 on error.
 * On success, *out_tuples is set to a palloc'd array of HeapTuple
 * pointers (NULL if zero rows).  On error, *out_tuples is unchanged.
 *
 * This is the natural complement to ladybug_bridge_pushed_sql() for
 * queries where the planner has no pushdown SQL to extract (e.g.,
 * "RETURN 1") and the cypher must be executed as-is via the engine.
 */
int
ladybug_bridge_execute_collect(LadybugBridge *b,
                                const char *query,
                                TupleDesc tupdesc,
                                HeapTuple **out_tuples,
                                const char **err_msg)
{
    lbug_query_result result;
    lbug_state st;
    char *lbug_err;
    int num_cols;
    int natts;
    HeapTuple *tuples = NULL;
    int ntuples = 0;
    int tuples_alloc = 0;

    /* Defensive defaults so we can use *out_tuples safely on any return. */
    if (out_tuples)
        *out_tuples = NULL;

    if (b == NULL || !b->inited)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: bridge not initialized");
        return -1;
    }
    if (query == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: query is NULL");
        return -1;
    }
    if (tupdesc == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: tupdesc is NULL");
        return -1;
    }
    if (out_tuples == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: out_tuples is NULL");
        return -1;
    }

    memset(&result, 0, sizeof(result));

    st = lbug_connection_query(&b->connection, query, &result);
    if (st != 0 || !lbug_query_result_is_success(&result))
    {
        lbug_err = lbug_query_result_get_error_message(&result);
        if (err_msg)
            *err_msg = psprintf("ladybug: query failed: %s",
                                lbug_err ? lbug_err : "(no error message)");
        if (lbug_err)
            lbug_destroy_string(lbug_err);
        lbug_query_result_destroy(&result);
        return -1;
    }

    num_cols = (int)lbug_query_result_get_num_columns(&result);
    natts = tupdesc->natts;

    if (num_cols != natts)
    {
        if (err_msg)
            *err_msg = psprintf("ladybug: column count mismatch: query returns %d columns, expected %d",
                                num_cols, natts);
        lbug_query_result_destroy(&result);
        return -1;
    }

    while (lbug_query_result_has_next(&result))
    {
        lbug_flat_tuple flat_tuple;
        Datum *values;
        bool  *nulls;
        HeapTuple tuple;

        memset(&flat_tuple, 0, sizeof(flat_tuple));

        st = lbug_query_result_get_next(&result, &flat_tuple);
        if (st != 0)
        {
            if (err_msg)
                *err_msg = pstrdup("ladybug: error fetching next tuple");
            if (tuples)
            {
                for (int i = 0; i < ntuples; i++)
                    pfree(tuples[i]);
                pfree(tuples);
            }
            lbug_query_result_destroy(&result);
            *out_tuples = NULL;
            return -1;
        }

        values = (Datum *) palloc0(natts * sizeof(Datum));
        nulls  = (bool  *) palloc0(natts * sizeof(bool));

        for (int a = 0; a < natts; a++)
        {
            lbug_value val;
            char *str;

            memset(&val, 0, sizeof(val));

            st = lbug_flat_tuple_get_value(&flat_tuple, (uint64_t)a, &val);
            if (st != 0 || lbug_value_is_null(&val))
            {
                nulls[a] = true;
                values[a] = (Datum) 0;
                continue;
            }

            str = NULL;
            st = lbug_value_get_string(&val, &str);
            if (st != 0 || str == NULL)
                str = lbug_value_to_string(&val);

            if (str != NULL)
            {
                Oid typid, typioparam, typinput;
                int32 typmod;
                FmgrInfo finfo;

                typid = TupleDescAttr(tupdesc, a)->atttypid;
                typmod = TupleDescAttr(tupdesc, a)->atttypmod;
                getTypeInputInfo(typid, &typinput, &typioparam);
                fmgr_info(typinput, &finfo);

                values[a] = InputFunctionCall(&finfo, str, typioparam, typmod);
                nulls[a] = false;

                lbug_destroy_string(str);
            }
            else
            {
                nulls[a] = true;
                values[a] = (Datum) 0;
            }
        }

        tuple = heap_form_tuple(tupdesc, values, nulls);
        pfree(values);
        pfree(nulls);

        /*
         * Grow the output array as needed (start at 8, double
         * thereafter).  We must use palloc() for the very first
         * allocation: PG's repalloc() asserts on a NULL pointer in
         * some builds, while it works fine in others.  palloc() is
         * always safe and equivalent for the empty -> non-empty
         * transition.
         */
        if (ntuples >= tuples_alloc)
        {
            int new_alloc = tuples_alloc ? tuples_alloc * 2 : 8;
            HeapTuple *new_tuples;

            if (tuples == NULL)
                new_tuples = (HeapTuple *) palloc(new_alloc * sizeof(HeapTuple));
            else
                new_tuples = (HeapTuple *) repalloc(tuples,
                                                   new_alloc * sizeof(HeapTuple));
            tuples = new_tuples;
            tuples_alloc = new_alloc;
        }
        tuples[ntuples++] = tuple;
    }

    lbug_query_result_destroy(&result);
    *out_tuples = tuples;
    return ntuples;
}

void
ladybug_bridge_release(LadybugBridge *b)
{
    if (b == NULL || !b->inited)
        return;

    lbug_connection_destroy(&b->connection);
    lbug_database_destroy(&b->database);

    if (bridge.attached_connstr)
        pfree(bridge.attached_connstr);
    memset(&bridge, 0, sizeof(bridge));
}