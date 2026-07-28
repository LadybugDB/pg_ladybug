/*
 * pg_ladybug.c
 *
 * PostgreSQL extension: Cypher query support through the embedded
 * Ladybug graph engine (liblbug).
 *
 * Architecture:
 *   - ladybug_cypher(text)   -> SETOF record
 *       1. Create in-memory Ladybug database + connection (compile-time linked)
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
 * Ladybug is used purely as a Cypher -> SQL compiler; the pushed-down SQL
 * is executed natively by PostgreSQL itself.
 *
 * Build-time linking: pg_ladybug links directly to liblbug.so (-llbug)
 * rather than using dlopen().  This is best practice for PG extensions
 * and ensures the library is loaded by the PG backend's
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
#include "miscadmin.h"              /* DataDir */
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
extern int    ladybug_bridge_execute_collect(LadybugBridge *b,
                                             const char *query,
                                             TupleDesc tupdesc,
                                             HeapTuple **out_tuples,
                                             const char **err_msg);
extern void   ladybug_bridge_release(LadybugBridge *b);

/* ------------------------------------------------------------------ */
/* GUCs                                                               */
/* ------------------------------------------------------------------ */
static char *ladybug_pg_connstr = "";
static char *ladybug_storage_path = NULL;

/*
 * Default storage path, computed in _PG_init from DataDir.
 * The Ladybug engine uses this path for persistent storage of
 * its catalog (table/schema metadata, not graph data). The
 * bridge tries to initialize at this path and falls back to
 * in-memory mode if the path is empty or initialization fails.
 *
 * Format: <DataDir>/storage.lbdb
 */
#define LADYBUG_DEFAULT_STORAGE_PATH_BUFSIZE 1024
static char ladybug_default_storage_path[LADYBUG_DEFAULT_STORAGE_PATH_BUFSIZE];

/* ------------------------------------------------------------------ */
/* SRF execution context for row streaming                             */
/* ------------------------------------------------------------------ */
#define MAX_COLS 64
typedef struct LadybugSRFContext
{
    TupleDesc       tupdesc;
    int             natts;
    int             current_row;  /* current row index while iterating */
    int             nrows;        /* total rows */
    Datum          *values[MAX_COLS]; /* per-row Datum arrays */
    bool           *nulls[MAX_COLS];  /* per-row null flags */
} LadybugSRFContext;

/* ------------------------------------------------------------------ */
/* Core: run SQL via SPI and return the SPITupleTable for direct use    */
/* Returns the number of rows, or -1 on error.                          */
/* ------------------------------------------------------------------ */
static int
spi_execute_and_capture(const char *sql, TupleDesc expected_tupdesc,
                        SPITupleTable **out_tuptable, int *out_ncols)
{
    int ret;

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

    *out_tuptable = SPI_tuptable;
    if (out_ncols)
        *out_ncols = SPI_tuptable ? SPI_tuptable->tupdesc->natts : 0;

    return (int) SPI_processed;
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

    /* Defense-in-depth: should not be reached because SQL declares STRICT */
    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errmsg("ladybug.cypher(): NULL argument is not allowed")));

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext oldcontext;
        text    *cypher_text = PG_GETARG_TEXT_PP(0);
        char    *cypher_cstr;
        const char *err_msg = NULL;
        char    *sql;
        LadybugBridge *b;
        int nrows = 0;
        int ncols = 0;
        SPITupleTable *tuptable;

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
        srfctx->current_row = 0;
        srfctx->nrows = 0;
        funcctx->user_fctx = srfctx;

        /* Acquire the Ladybug bridge */
        b = ladybug_bridge_acquire(&err_msg);
        if (b == NULL)
        {
            MemoryContextSwitchTo(oldcontext);
            if (err_msg)
                ereport(ERROR,
                        (errmsg("ladybug: cannot initialize Ladybug engine"),
                         errdetail("%s", err_msg)));
            else
                ereport(ERROR,
                        (errmsg("ladybug: cannot initialize Ladybug engine")));
        }

        /* ATTACH local Postgres (idempotent) */
        ensure_attached(b);

        /* Translate Cypher -> pushed-down SQL */
        cypher_cstr = text_to_cstring(cypher_text);
        err_msg = NULL;
        sql = ladybug_bridge_pushed_sql(b, cypher_cstr, &err_msg);

        if (sql != NULL)
        {
            /* ----------------------------------------------------------------
             * Pushdown available: execute the SQL via native SPI on this
             * Postgres backend.
             * ----------------------------------------------------------------
             */
            pfree(cypher_cstr);

            /* Execute SQL via SPI and copy all row data into multi-call memory context */
            nrows = spi_execute_and_capture(sql, expected_tupdesc, &tuptable, &ncols);
            pfree(sql);

            srfctx->nrows = nrows;
            funcctx->max_calls = nrows;
            /*
             * Build HeapTuples BEFORE SPI_finish, because SPI_getbinval may
             * return pointers into SPI memory that would be freed by SPI_finish.
             * Store the Datum representation of each HeapTuple in multi-call ctx.
             */
            if (nrows > 0 && tuptable != NULL)
            {
                TupleDesc spi_tupdesc = tuptable->tupdesc;
                int natts = expected_tupdesc->natts;
                /* Build column name mapping from SPI result columns to expected.
                 * Strategy:
                 * 1. Try exact name match first (column aliases match exactly).
                 * 2. If no exact match, try suffix match: for SPI columns like
                 *    "k_since", strip the table-prefix part (before '_') and match
                 *    the remainder against the expected name. This handles cases
                 *    where the SQL aliases are prefixed (e.g. "k_since") but the
                 *    expected column name is the bare property name (e.g. "since").
                 * 3. If still no match, try matching just the bare property name
                 *    from the SPI column (everything after the first '_' or '.').
                 */
                int *col_map = (int *) palloc(natts * sizeof(int));
                for (int a = 0; a < natts; a++)
                {
                    const char *exp_name;

                    col_map[a] = -1;
                    exp_name = NameStr(TupleDescAttr(expected_tupdesc, a)->attname);

                    /* Pass 1: exact match */
                    for (int b = 0; b < ncols; b++)
                    {
                        const char *spi_name = NameStr(TupleDescAttr(spi_tupdesc, b)->attname);
                        if (strcmp(exp_name, spi_name) == 0)
                        {
                            col_map[a] = b;
                            break;
                        }
                    }

                    /* Pass 2: suffix match - spi_name ends with '_' + exp_name */
                    if (col_map[a] < 0)
                    {
                        size_t exp_len = strlen(exp_name);
                        for (int b = 0; b < ncols; b++)
                        {
                            const char *spi_name = NameStr(TupleDescAttr(spi_tupdesc, b)->attname);
                            size_t spi_len = strlen(spi_name);
                            if (spi_len > exp_len + 1 &&
                                spi_name[spi_len - exp_len - 1] == '_' &&
                                strcmp(spi_name + spi_len - exp_len, exp_name) == 0)
                            {
                                col_map[a] = b;
                                break;
                            }
                        }
                    }

                    /* Pass 3: match bare property name from SPI column */
                    if (col_map[a] < 0)
                    {
                        for (int b = 0; b < ncols; b++)
                        {
                            const char *spi_name = NameStr(TupleDescAttr(spi_tupdesc, b)->attname);
                            const char *underscore = strchr(spi_name, '_');
                            const char *dot = strchr(spi_name, '.');
                            const char *sep = (underscore && dot) ?
                                (underscore < dot ? underscore : dot) :
                                (underscore ? underscore : dot);
                            if (sep != NULL && strcmp(sep + 1, exp_name) == 0)
                            {
                                col_map[a] = b;
                                break;
                            }
                        }
                    }

                    /* If still no match, leave col_map[a] = -1 (will yield NULL) */
                }

                for (int i = 0; i < nrows && i < MAX_COLS; i++)
                {
                    HeapTuple spi_tup;
                    Datum *vals;
                    bool  *nls;
                    HeapTuple ht;

                    spi_tup = tuptable->vals[i];
                    vals = (Datum *) palloc(natts * sizeof(Datum));
                    nls  = (bool  *) palloc(natts * sizeof(bool));

                    for (int a = 0; a < natts; a++)
                    {
                        int spi_idx;
                        bool isnull;

                        spi_idx = col_map[a];
                        isnull = false;
                        if (spi_idx >= 0 && spi_idx < ncols)
                            vals[a] = SPI_getbinval(spi_tup, spi_tupdesc, spi_idx + 1, &isnull);
                        else
                            isnull = true;
                        nls[a] = isnull;
                    }

                    /* Build the heap tuple now, while SPI data is still valid */
                    ht = heap_form_tuple(expected_tupdesc, vals, nls);
                    srfctx->values[i] = (Datum *) palloc(sizeof(Datum));
                    srfctx->values[i][0] = HeapTupleGetDatum(ht);
                    srfctx->nulls[i] = NULL;

                    pfree(vals);
                    pfree(nls);
                }
                pfree(col_map);
            }

            SPI_finish();
        }
        else
        {
            /* ----------------------------------------------------------------
             * No pushdown available (e.g., a query like "RETURN 1" that has
             * no SQL to push down, or a complex query the planner chose not
             * to push down).  Fall back to executing the cypher as-is via
             * the Ladybug engine and stream the result rows back as if the
             * corresponding native SQL had been executed.
             *
             * Discard the informational err_msg from pushed_sql; the bridge
             * sets it whenever the function returns NULL, regardless of
             * whether the cause was "no pushdown" or a real error.  Any
             * real error will surface again from the direct execution below.
             * ----------------------------------------------------------------
             */
            HeapTuple *tuples = NULL;
            int       nrows_lbug;

            if (err_msg)
            {
                pfree((char *) err_msg);
                err_msg = NULL;
            }

            err_msg = NULL;
            nrows_lbug = ladybug_bridge_execute_collect(
                                            b, cypher_cstr,
                                            expected_tupdesc,
                                            &tuples, &err_msg);
            pfree(cypher_cstr);

            if (nrows_lbug < 0)
            {
                MemoryContextSwitchTo(oldcontext);
                if (err_msg)
                    ereport(ERROR,
                            (errmsg("ladybug: failed to execute cypher via "
                                    "Ladybug engine (no pushdown SQL was "
                                    "available and direct execution failed)"),
                             errdetail("%s", err_msg),
                             errhint("Use ladybug.explain() to inspect the "
                                     "plan, or ladybug.pushed_sql() to see "
                                     "what SQL (if any) the planner would "
                                     "have pushed down.")));
                else
                    ereport(ERROR,
                            (errmsg("ladybug: failed to execute cypher via "
                                    "Ladybug engine (no pushdown SQL was "
                                    "available and direct execution failed)")));
                /* ereport(ERROR) does not return */
            }

            srfctx->nrows = nrows_lbug;
            funcctx->max_calls = nrows_lbug;

            /*
             * Wrap each returned HeapTuple in a single-element Datum array,
             * matching the layout produced by the SPI path above so the
             * SRF per-call code can use a single accessor.
             *
             * The HeapTuples are already palloc'd in the multi-call memory
             * context (we set that context before calling the bridge), so
             * they survive until the SRF completes.
             */
            for (int i = 0; i < nrows_lbug && i < MAX_COLS; i++)
            {
                srfctx->values[i] = (Datum *) palloc(sizeof(Datum));
                srfctx->values[i][0] = HeapTupleGetDatum(tuples[i]);
                srfctx->nulls[i] = NULL;
            }
            if (tuples != NULL)
                pfree(tuples);
        }

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    srfctx = (LadybugSRFContext *) funcctx->user_fctx;

    if (srfctx->current_row < srfctx->nrows && srfctx->current_row < MAX_COLS)
    {
        int i = srfctx->current_row;
        Datum result = srfctx->values[i][0];
        srfctx->current_row++;
        SRF_RETURN_NEXT(funcctx, result);
    }

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
    text    *query_text;
    char    *query_cstr;

    /* Defense-in-depth: should not be reached because SQL declares STRICT */
    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errmsg("ladybug.sql_query(): NULL argument is not allowed")));

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext oldcontext;
        int nrows = 0;
        int ncols = 0;
        SPITupleTable *tuptable;

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
        srfctx->current_row = 0;
        srfctx->nrows = 0;
        funcctx->user_fctx = srfctx;
        funcctx->max_calls = 0;

        query_text = PG_GETARG_TEXT_PP(0);
        query_cstr = text_to_cstring(query_text);

        nrows = spi_execute_and_capture(query_cstr, expected_tupdesc, &tuptable, &ncols);
        pfree(query_cstr);

        srfctx->nrows = nrows;
        funcctx->max_calls = nrows;

        /*
         * Build HeapTuples BEFORE SPI_finish, because SPI_getbinval may
         * return pointers into SPI memory that would be freed by SPI_finish.
         */
        if (nrows > 0 && tuptable != NULL)
        {
            TupleDesc spi_tupdesc = tuptable->tupdesc;
            int natts = expected_tupdesc->natts;
            /* Build column name mapping from SPI result columns to expected.
             * Strategy:
             * 1. Try exact name match first (column aliases match exactly).
             * 2. If no exact match, try suffix match: for SPI columns like
             *    "k_since", strip the table-prefix part (before '_') and match
             *    the remainder against the expected name. This handles cases
             *    where the SQL aliases are prefixed (e.g. "k_since") but the
             *    expected column name is the bare property name (e.g. "since").
             * 3. If still no match, try matching just the bare property name
             *    from the SPI column (everything after the first '_' or '.').
             */
            int *col_map = (int *) palloc(natts * sizeof(int));
            for (int a = 0; a < natts; a++)
            {
                const char *exp_name;

                col_map[a] = -1;
                exp_name = NameStr(TupleDescAttr(expected_tupdesc, a)->attname);

                /* Pass 1: exact match */
                for (int b = 0; b < ncols; b++)
                {
                    const char *spi_name = NameStr(TupleDescAttr(spi_tupdesc, b)->attname);
                    if (strcmp(exp_name, spi_name) == 0)
                    {
                        col_map[a] = b;
                        break;
                    }
                }

                /* Pass 2: suffix match - spi_name ends with '_' + exp_name */
                if (col_map[a] < 0)
                {
                    size_t exp_len = strlen(exp_name);
                    for (int b = 0; b < ncols; b++)
                    {
                        const char *spi_name = NameStr(TupleDescAttr(spi_tupdesc, b)->attname);
                        size_t spi_len = strlen(spi_name);
                        if (spi_len > exp_len + 1 &&
                            spi_name[spi_len - exp_len - 1] == '_' &&
                            strcmp(spi_name + spi_len - exp_len, exp_name) == 0)
                        {
                            col_map[a] = b;
                            break;
                        }
                    }
                }

                /* Pass 3: match bare property name from SPI column */
                if (col_map[a] < 0)
                {
                    for (int b = 0; b < ncols; b++)
                    {
                        const char *spi_name = NameStr(TupleDescAttr(spi_tupdesc, b)->attname);
                        const char *underscore = strchr(spi_name, '_');
                        const char *dot = strchr(spi_name, '.');
                        const char *sep = (underscore && dot) ?
                            (underscore < dot ? underscore : dot) :
                            (underscore ? underscore : dot);
                        if (sep != NULL && strcmp(sep + 1, exp_name) == 0)
                        {
                            col_map[a] = b;
                            break;
                        }
                    }
                }

                /* If still no match, leave col_map[a] = -1 (will yield NULL) */
            }

            for (int i = 0; i < nrows && i < MAX_COLS; i++)
            {
                HeapTuple spi_tup;
                Datum *vals;
                bool  *nls;
                HeapTuple ht;

                spi_tup = tuptable->vals[i];
                vals = (Datum *) palloc(natts * sizeof(Datum));
                nls  = (bool  *) palloc(natts * sizeof(bool));

                for (int a = 0; a < natts; a++)
                {
                    int spi_idx;
                    bool isnull;

                    spi_idx = col_map[a];
                    isnull = false;
                    if (spi_idx >= 0 && spi_idx < ncols)
                        vals[a] = SPI_getbinval(spi_tup, spi_tupdesc, spi_idx + 1, &isnull);
                    else
                        isnull = true;
                    nls[a] = isnull;
                }

                /* Build the heap tuple now, while SPI data is still valid */
                ht = heap_form_tuple(expected_tupdesc, vals, nls);
                srfctx->values[i] = (Datum *) palloc(sizeof(Datum));
                srfctx->values[i][0] = HeapTupleGetDatum(ht);
                srfctx->nulls[i] = NULL;

                pfree(vals);
                pfree(nls);
            }
            pfree(col_map);
        }

        SPI_finish();
        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    srfctx = (LadybugSRFContext *) funcctx->user_fctx;

    if (srfctx->current_row < srfctx->nrows && srfctx->current_row < MAX_COLS)
    {
        int i = srfctx->current_row;
        Datum result = srfctx->values[i][0];
        srfctx->current_row++;
        SRF_RETURN_NEXT(funcctx, result);
    }

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
    text     *cypher_text;
    char     *cypher_cstr;
    const char *err_msg = NULL;
    char     *plan_text;
    text     *result;
    LadybugBridge *b;

    /* Defense-in-depth: should not be reached because SQL declares STRICT */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    cypher_text = PG_GETARG_TEXT_PP(0);

    LadybugBridge *b = ladybug_bridge_acquire(&err_msg);
    if (b == NULL)
    {
        if (err_msg)
            ereport(ERROR,
                    (errmsg("ladybug: cannot initialize Ladybug engine"),
                     errdetail("%s", err_msg)));
        else
            ereport(ERROR,
                    (errmsg("ladybug: cannot initialize Ladybug engine")));
    }

    /* Ensure ATTACH has happened so the planner can resolve tables */
    ensure_attached(b);

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
    text     *cypher_text;
    char     *cypher_cstr;
    const char *err_msg = NULL;
    char     *sql;
    text     *result;
    LadybugBridge *b;

    /* Defense-in-depth: should not be reached because SQL declares STRICT */
    if (PG_ARGISNULL(0))
        PG_RETURN_NULL();

    cypher_text = PG_GETARG_TEXT_PP(0);
    b = ladybug_bridge_acquire(&err_msg);
    if (b == NULL)
    {
        if (err_msg)
            ereport(ERROR,
                    (errmsg("ladybug: cannot initialize Ladybug engine"),
                     errdetail("%s", err_msg)));
        else
            ereport(ERROR,
                    (errmsg("ladybug: cannot initialize Ladybug engine")));
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
    /*
     * Compute the default storage path: <DataDir>/storage.lbdb.
     * The static buffer is used as the bootValue for the GUC; the GUC
     * framework copies it to a palloc'd buffer it owns, so the static
     * buffer is never freed.
     */
    {
        const char *dir = DataDir;
        size_t      len = (dir != NULL) ? strlen(dir) : 0;

        if (len == 0)
        {
            /* No DataDir available; fall back to a relative path. */
            snprintf(ladybug_default_storage_path,
                     sizeof(ladybug_default_storage_path),
                     "storage.lbdb");
        }
        else if (dir[len - 1] == '/')
        {
            snprintf(ladybug_default_storage_path,
                     sizeof(ladybug_default_storage_path),
                     "%sstorage.lbdb", dir);
        }
        else
        {
            snprintf(ladybug_default_storage_path,
                     sizeof(ladybug_default_storage_path),
                     "%s/storage.lbdb", dir);
        }
    }

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

    DefineCustomStringVariable(
        "ladybug.storage_path",
        "Filesystem path for the Ladybug storage database. "
        "Defaults to <DataDir>/storage.lbdb, which gives each cluster "
        "its own persistent Ladybug catalog under the cluster's data "
        "directory. The Ladybug engine uses this path instead of the "
        "in-memory default. If initialization at this path fails, the "
        "extension falls back to in-memory mode and emits a WARNING.",
        "Set to an empty string to disable persistent storage (in-memory mode). "
        "The path is read once per backend on the first call to a function "
        "that needs the Ladybug engine; changes after that point do not "
        "affect the already-initialized bridge.",
        &ladybug_storage_path,
        ladybug_default_storage_path,
        PGC_USERSET,
        0,
        NULL, NULL, NULL);

    elog(LOG, "pg_ladybug: loaded (Cypher via embedded Ladybug engine + native SPI), default storage='%s'",
         ladybug_default_storage_path);
}