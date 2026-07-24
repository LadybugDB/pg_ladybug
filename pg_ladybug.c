/*
 * pg_ladybug.c
 *
 * PostgreSQL extension: Cypher query support through the embedded
 * Ladybug graph engine (liblbug).
 *
 * Architecture:
 *   - ladybug_cypher(text)   -> SETOF record
 *       1. dlopen liblbug (runtime; ladybug.lib_path GUC)
 *       2. ATTACH the current Postgres as a foreign catalog (ladybug.pg_connstr)
 *       3. EXPLAIN the Cypher in Ladybug
 *       4. Extract the pushed-down SQL from the plan text
 *       5. Execute that SQL natively via SPI against local Postgres tables
 *       6. Stream rows back as a set-returning function
 *   - ladybug_sql_query(text) -> SETOF record
 *       Pure native SPI executor (no Ladybug involvement).
 *   - ladybug_explain(text)   -> text
 *       Return the raw EXPLAIN plan text from Ladybug.
 *   - ladybug_pushed_sql(text) -> text
 *       Return the extracted pushed-down SQL string.
 *
 * No DuckDB runs inside the PostgreSQL backend.  Ladybug is used purely
 * as a Cypher -> SQL compiler; the pushed-down SQL is executed natively
 * by PostgreSQL itself.
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "utils/tuplestore.h"
#include "catalog/pg_type.h"
#include "access/htup_details.h"
#include "nodes/execnodes.h"
#include <string.h>

PG_MODULE_MAGIC;

/* ------------------------------------------------------------------ */
/* Forward declarations from ladybug_bridge.c                          */
/* ------------------------------------------------------------------ */
typedef struct LadybugBridge LadybugBridge;

extern LadybugBridge *ladybug_bridge_acquire(const char **err_msg);
extern char  *ladybug_bridge_pushed_sql(LadybugBridge *b, const char *cypher,
                                        const char **err_msg);
extern char  *ladybug_bridge_explain(LadybugBridge *b, const char *cypher,
                                     const char **err_msg);
extern char  *ladybug_bridge_direct_sql(LadybugBridge *b, const char *sql,
                                        const char **err_msg);
extern bool   ladybug_bridge_attach_postgres(LadybugBridge *b,
                                             const char *pg_connstr,
                                             const char **err_msg);
extern void   ladybug_bridge_release(LadybugBridge *b);

/* ------------------------------------------------------------------ */
/* GUCs                                                               */
/* ------------------------------------------------------------------ */
static char *ladybug_lib_path  = "liblbug.so";
static char *ladybug_pg_connstr = "";

/* ------------------------------------------------------------------ */
/* SRF execution context for row streaming                             */
/* ------------------------------------------------------------------ */
typedef struct LadybugSRFContext
{
    Tuplestorestate *tuplestore;
    TupleDesc        tupdesc;
    int              natts;
} LadybugSRFContext;

/* ------------------------------------------------------------------ */
/* Core: run SQL via SPI and copy rows into a tuplestore               */
/* ------------------------------------------------------------------ */
static Tuplestorestate *
spi_select_into_tuplestore(const char *sql, TupleDesc expected_tupdesc,
                           int *out_nrows)
{
    int ret;
    SPITupleTable *tuptable;
    Tuplestorestate *ts;
    int nrows;
    int natts;

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        ereport(ERROR,
                (errmsg("ladybug: SPI_connect failed")));

    ret = SPI_execute(sql, true /* read_only */, 0 /* no limit */);
    if (ret != SPI_OK_SELECT &&
        ret != SPI_OK_INSERT_RETURNING &&
        ret != SPI_OK_UPDATE_RETURNING &&
        ret != SPI_OK_DELETE_RETURNING)
    {
        SPI_finish();
        ereport(ERROR,
                (errmsg("ladybug: SPI_execute failed for pushed-down SQL"),
                 errdetail("SPI status: %d", ret),
                 errhint("The pushed-down SQL: %s", sql)));
    }

    tuptable = SPI_tuptable;
    nrows = (int) SPI_processed;
    natts = tuptable->tupdesc->natts;

    /*
     * Build the tuplestore in the multi-call memory context so it survives
     * across SRF per-call iterations.  We copy each SPI row out of the SPI
     * memory context into heap-form tuples and put them in the store.
     */
    ts = tuplestore_begin_heap(false /* randomAccess */,
                               false /* interXact */,
                               1024 /* work_mem_bytes */);

    for (int i = 0; i < nrows; i++)
    {
        HeapTuple spi_tup = tuptable->vals[i];
        TupleDesc spi_tupdesc = tuptable->tupdesc;
        Datum    *values;
        bool     *nulls;

        HeapTuple result;
        values = (Datum *) palloc0(natts * sizeof(Datum));
        nulls  = (bool  *) palloc0(natts * sizeof(bool));

        for (int a = 0; a < natts; a++)
        {
            bool isnull = false;
            values[a] = SPI_getbinval(spi_tup, spi_tupdesc, a + 1, &isnull);
            nulls[a] = isnull;
        }

        /*
         * If the SPI tuple descriptor matches the expected (caller-provided)
         * tuple descriptor column-for-column by type, use the expected
         * tupdesc for the stored tuple; otherwise use the SPI tupdesc.
         * In practice the pushed-down SQL projection should match the
         * caller's AS t(...) declaration.
         */
        if (expected_tupdesc &&
            expected_tupdesc->natts == natts)
        {
            result = heap_form_tuple(expected_tupdesc, values, nulls);
        }
        else
        {
            result = heap_form_tuple(spi_tupdesc, values, nulls);
        }
        tuplestore_puttuple(ts, result);
        heap_freetuple(result);

        pfree(values);
        pfree(nulls);
    }

    SPI_finish();

    if (out_nrows)
        *out_nrows = nrows;
    return ts;
}

/* ------------------------------------------------------------------ */
/* Ensure the bridge has ATTACHed this Postgres (once per backend)       */
/* ------------------------------------------------------------------ */
static void
ensure_attached(LadybugBridge *b)
{
    const char *connstr = ladybug_pg_connstr;

    if (b == NULL)
        ereport(ERROR,
                (errmsg("ladybug: bridge not available")));

    if (connstr == NULL || connstr[0] == '\0')
        ereport(ERROR,
                (errmsg("ladybug: ladybug.pg_connstr is not set"),
                 errhint("Set ladybug.pg_connstr to a Postgres connection "
                         "string for the current database, e.g. "
                         "'host=localhost port=5432 dbname=mydb user=me'.")));

    {
        const char *e = NULL;
        if (!ladybug_bridge_attach_postgres(b, connstr, &e))
        {
            if (e)
                ereport(ERROR,
                        (errmsg("ladybug: failed to ATTACH local Postgres"),
                         errdetail("%s", e)));
            else
                ereport(ERROR,
                        (errmsg("ladybug: failed to ATTACH local Postgres")));
            /* ereport(ERROR) does not return */
        }
    }
}

/* ------------------------------------------------------------------ */
/* ladybug_cypher(text) -> SETOF record                                */
/* ------------------------------------------------------------------ */
PG_FUNCTION_INFO_V1(ladybug_cypher);

Datum
ladybug_cypher(PG_FUNCTION_ARGS)
{
    FuncCallContext    *funcctx;
    LadybugSRFContext  *srfctx;
    TupleDesc           expected_tupdesc;
    Tuplestorestate    *ts;
    TupleTableSlot     *slot;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext oldcontext;
        text    *cypher_text = PG_GETARG_TEXT_PP(0);
        char    *cypher_cstr;
        const char *err_msg = NULL;
        char    *sql;
        LadybugBridge *b;
        int nrows = 0;

        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        /* Determine the caller's expected tuple descriptor */
        if (get_call_result_type(fcinfo, NULL, &expected_tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errmsg("ladybug.cypher() requires a column definition list"),
                     errhint("Add an \"AS t(col type, ...)\" clause.")));

        BlessTupleDesc(expected_tupdesc);

        srfctx = (LadybugSRFContext *) palloc0(sizeof(LadybugSRFContext));
        srfctx->tupdesc = expected_tupdesc;
        srfctx->natts = expected_tupdesc->natts;
        funcctx->max_calls = 0;   /* we use tuplestore, not max_calls */
        funcctx->user_fctx = srfctx;

        /* Acquire the Ladybug bridge */
        b = ladybug_bridge_acquire(&err_msg);
        if (b == NULL)
        {
            MemoryContextSwitchTo(oldcontext);
            if (err_msg)
                ereport(ERROR,
                        (errmsg("ladybug: cannot load Ladybug engine"),
                         errdetail("%s", err_msg)));
            else
                ereport(ERROR,
                        (errmsg("ladybug: cannot load Ladybug engine")));
        }

        /* ATTACH local Postgres (idempotent) */
        ensure_attached(b);

        /* Translate Cypher -> pushed-down SQL */
        cypher_cstr = text_to_cstring(cypher_text);
        err_msg = NULL;
        sql = ladybug_bridge_pushed_sql(b, cypher_cstr, &err_msg);
        pfree(cypher_cstr);
        if (sql == NULL)
        {
            MemoryContextSwitchTo(oldcontext);
            if (err_msg)
                ereport(ERROR,
                        (errmsg("ladybug: could not extract pushed-down SQL"),
                         errdetail("%s", err_msg),
                         errhint("Use ladybug.explain() or ladybug.pushed_sql() to inspect.")));
            else
                ereport(ERROR,
                        (errmsg("ladybug: could not extract pushed-down SQL")));
        }

        elog(DEBUG1, "ladybug: pushed-down SQL: %s", sql);

        /* Execute the SQL natively via SPI and fill a tuplestore */
        ts = spi_select_into_tuplestore(sql, expected_tupdesc, &nrows);
        pfree(sql);

        srfctx->tuplestore = ts;
        funcctx->max_calls = nrows;

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    srfctx = (LadybugSRFContext *) funcctx->user_fctx;
    ts = srfctx->tuplestore;

    /*
     * Use a slot compatible with the expected tupdesc to drain the store.
     */
    slot = MakeSingleTupleTableSlot(srfctx->tupdesc, &TTSOpsHeapTuple);
    if (tuplestore_gettupleslot(ts, true /* forward */, false, slot))
    {
        Datum result = ExecFetchSlotHeapTupleDatum(slot);
        ExecClearTuple(slot);
        ExecDropSingleTupleTableSlot(slot);
        SRF_RETURN_NEXT(funcctx, result);
    }

    ExecDropSingleTupleTableSlot(slot);
    SRF_RETURN_DONE(funcctx);
}

/* ------------------------------------------------------------------ */
/* ladybug_sql_query(text) -> SETOF record                             */
/* Pure native SPI executor (no Ladybug).                             */
/* ------------------------------------------------------------------ */
PG_FUNCTION_INFO_V1(ladybug_sql_query);

Datum
ladybug_sql_query(PG_FUNCTION_ARGS)
{
    FuncCallContext    *funcctx;
    LadybugSRFContext  *srfctx;
    TupleDesc           expected_tupdesc;
    Tuplestorestate    *ts;
    TupleTableSlot     *slot;
    text    *query_text;
    char    *query_cstr;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext oldcontext;
        int nrows = 0;

        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        if (get_call_result_type(fcinfo, NULL, &expected_tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errmsg("ladybug.sql_query() requires a column definition list"),
                     errhint("Add an \"AS t(col type, ...)\" clause.")));

        BlessTupleDesc(expected_tupdesc);

        srfctx = (LadybugSRFContext *) palloc0(sizeof(LadybugSRFContext));
        srfctx->tupdesc = expected_tupdesc;
        srfctx->natts = expected_tupdesc->natts;
        funcctx->user_fctx = srfctx;
        funcctx->max_calls = 0;

        query_text = PG_GETARG_TEXT_PP(0);
        query_cstr = text_to_cstring(query_text);

        ts = spi_select_into_tuplestore(query_cstr, expected_tupdesc, &nrows);
        pfree(query_cstr);

        srfctx->tuplestore = ts;
        funcctx->max_calls = nrows;

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    srfctx = (LadybugSRFContext *) funcctx->user_fctx;
    ts = srfctx->tuplestore;

    slot = MakeSingleTupleTableSlot(srfctx->tupdesc, &TTSOpsHeapTuple);
    if (tuplestore_gettupleslot(ts, true, false, slot))
    {
        Datum result = ExecFetchSlotHeapTupleDatum(slot);
        ExecClearTuple(slot);
        ExecDropSingleTupleTableSlot(slot);
        SRF_RETURN_NEXT(funcctx, result);
    }

    ExecDropSingleTupleTableSlot(slot);
    SRF_RETURN_DONE(funcctx);
}

/* ------------------------------------------------------------------ */
/* ladybug_explain(text) -> text                                       */
/* Returns the raw Ladybug EXPLAIN plan text.                          */
/* ------------------------------------------------------------------ */
PG_FUNCTION_INFO_V1(ladybug_explain);

Datum
ladybug_explain(PG_FUNCTION_ARGS)
{
    text     *cypher_text = PG_GETARG_TEXT_PP(0);
    char     *cypher_cstr;
    const char *err_msg = NULL;
    char     *plan_text;
    text     *result;

    LadybugBridge *b = ladybug_bridge_acquire(&err_msg);
    if (b == NULL)
    {
        if (err_msg)
            ereport(ERROR,
                    (errmsg("ladybug: cannot load Ladybug engine"),
                     errdetail("%s", err_msg)));
        else
            ereport(ERROR,
                    (errmsg("ladybug: cannot load Ladybug engine")));
    }

    cypher_cstr = text_to_cstring(cypher_text);
    plan_text = ladybug_bridge_explain(b, cypher_cstr, &err_msg);
    pfree(cypher_cstr);

    if (plan_text == NULL)
    {
        if (err_msg)
            ereport(ERROR,
                    (errmsg("ladybug: EXPLAIN failed"),
                     errdetail("%s", err_msg)));
        else
            ereport(ERROR,
                    (errmsg("ladybug: EXPLAIN failed")));
    }

    result = cstring_to_text(plan_text);
    pfree(plan_text);

    PG_RETURN_TEXT_P(result);
}

/* ------------------------------------------------------------------ */
/* ladybug_pushed_sql(text) -> text                                    */
/* Returns the extracted pushed-down SQL string.                       */
/* ------------------------------------------------------------------ */
PG_FUNCTION_INFO_V1(ladybug_pushed_sql);

Datum
ladybug_pushed_sql(PG_FUNCTION_ARGS)
{
    text     *cypher_text = PG_GETARG_TEXT_PP(0);
    char     *cypher_cstr;
    const char *err_msg = NULL;
    char     *sql;
    text     *result;

    LadybugBridge *b = ladybug_bridge_acquire(&err_msg);
    if (b == NULL)
    {
        if (err_msg)
            ereport(ERROR,
                    (errmsg("ladybug: cannot load Ladybug engine"),
                     errdetail("%s", err_msg)));
        else
            ereport(ERROR,
                    (errmsg("ladybug: cannot load Ladybug engine")));
    }

    /* Ensure ATTACH has happened so the planner can resolve tables */
    ensure_attached(b);

    cypher_cstr = text_to_cstring(cypher_text);
    sql = ladybug_bridge_pushed_sql(b, cypher_cstr, &err_msg);
    pfree(cypher_cstr);

    if (sql == NULL)
    {
        if (err_msg)
            ereport(ERROR,
                    (errmsg("ladybug: could not extract pushed-down SQL"),
                     errdetail("%s", err_msg),
                     errhint("Use ladybug.explain() to see the full plan.")));
        else
            ereport(ERROR,
                    (errmsg("ladybug: could not extract pushed-down SQL")));
    }

    result = cstring_to_text(sql);
    pfree(sql);

    PG_RETURN_TEXT_P(result);
}

/* ------------------------------------------------------------------ */
/* _PG_init: register GUCs                                             */
/* ------------------------------------------------------------------ */
void
_PG_init(void)
{
    DefineCustomStringVariable(
        "ladybug.lib_path",
        "Path to the Ladybug engine shared library (liblbug.so/liblbug.dylib).",
        "If set to just a bare name, dlopen will search the standard "
        "library search paths.",
        &ladybug_lib_path,
        "liblbug.so",
        PGC_USERSET,
        0,
        NULL, NULL, NULL);

    DefineCustomStringVariable(
        "ladybug.pg_connstr",
        "Postgres connection string for ATTACHing the current database "
        "to the Ladybug engine so its planner can resolve local tables.",
        "E.g. 'host=localhost port=5432 dbname=mydb user=me'. "
        "Must be set before calling ladybug.cypher() or ladybug.pushed_sql().",
        &ladybug_pg_connstr,
        "",
        PGC_USERSET,
        0,
        NULL, NULL, NULL);

    elog(LOG, "pg_ladybug: loaded (Cypher via embedded Ladybug engine + native SPI)");
}