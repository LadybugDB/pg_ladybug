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
#include "commands/trigger.h"
#include "nodes/execnodes.h"
#include "lib/stringinfo.h"
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
typedef struct LadybugSRFContext
{
    TupleDesc       tupdesc;
    int             natts;
    int             current_row;  /* current row index while iterating */
    int             nrows;        /* total rows */
    HeapTuple      *rows;         /* dynamically allocated array of HeapTuples */
} LadybugSRFContext;

/* ------------------------------------------------------------------ */
/* Core: run SQL via SPI and return the SPITupleTable for direct use    */
/* Returns the number of rows, or -1 on error.                          */
/* ------------------------------------------------------------------ */
static int
spi_execute_and_capture(const char *sql,
                        SPITupleTable **out_tuptable, int *out_ncols)
{
    int ret;

    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        ereport(ERROR,
                (errmsg("ladybug: SPI_connect failed")));

    /* read_only=true because we never execute modifying queries */
    ret = SPI_execute(sql, true, 0);
    if (ret != SPI_OK_SELECT)
    {
        SPI_finish();
        ereport(ERROR,
                (errmsg("ladybug: SPI_execute failed"),
                 errdetail("SPI status: %d", ret),
                 errhint("The SQL: %s", sql)));
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
            nrows = spi_execute_and_capture(sql, &tuptable, &ncols);
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
                                ereport(WARNING,
                                        (errmsg("ladybug: column \"%s\" matched to \"%s\" by suffix",
                                                exp_name, spi_name)));
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
                                ereport(WARNING,
                                        (errmsg("ladybug: column \"%s\" matched to \"%s\" by bare property name",
                                                exp_name, spi_name)));
                                break;
                            }
                        }
                    }

                    /* If still no match, leave col_map[a] = -1 (will yield NULL) */
                }

                srfctx->rows = (HeapTuple *) palloc(nrows * sizeof(HeapTuple));

                for (int i = 0; i < nrows; i++)
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
                        {
                            Oid spi_type = TupleDescAttr(spi_tupdesc, spi_idx)->atttypid;
                            Oid exp_type = TupleDescAttr(expected_tupdesc, a)->atttypid;
                            if (spi_type == exp_type)
                            {
                                vals[a] = SPI_getbinval(spi_tup, spi_tupdesc, spi_idx + 1, &isnull);
                            }
                            else
                            {
                                /* Type mismatch: convert through text representation */
                                char *str = SPI_getvalue(spi_tup, spi_tupdesc, spi_idx + 1);
                                if (str)
                                {
                                    Oid typioparam, typinput;
                                    int32 typmod;
                                    FmgrInfo finfo;
                                    getTypeInputInfo(exp_type, &typinput, &typioparam);
                                    typmod = TupleDescAttr(expected_tupdesc, a)->atttypmod;
                                    fmgr_info(typinput, &finfo);
                                    vals[a] = InputFunctionCall(&finfo, str, typioparam, typmod);
                                    pfree(str);
                                }
                                else
                                    isnull = true;
                            }
                        }
                        else
                            isnull = true;
                        nls[a] = isnull;
                    }

                    /* Build the heap tuple now, while SPI data is still valid */
                    ht = heap_form_tuple(expected_tupdesc, vals, nls);
                    srfctx->rows[i] = ht;

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
             * Take ownership of the HeapTuples returned by the bridge.
             * They are already palloc'd in the multi-call memory context,
             * so they survive until the SRF completes.
             */
            srfctx->rows = (HeapTuple *) palloc(nrows_lbug * sizeof(HeapTuple));
            for (int i = 0; i < nrows_lbug; i++)
                srfctx->rows[i] = tuples[i];
            if (tuples != NULL)
                pfree(tuples);
        }

        MemoryContextSwitchTo(oldcontext);
    }

    funcctx = SRF_PERCALL_SETUP();
    srfctx = (LadybugSRFContext *) funcctx->user_fctx;

    if (srfctx->current_row < srfctx->nrows)
    {
        int i = srfctx->current_row;
        Datum result = HeapTupleGetDatum(srfctx->rows[i]);
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

        nrows = spi_execute_and_capture(query_cstr, &tuptable, &ncols);
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
                            ereport(WARNING,
                                    (errmsg("ladybug: column \"%s\" matched to \"%s\" by suffix",
                                            exp_name, spi_name)));
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
                            ereport(WARNING,
                                    (errmsg("ladybug: column \"%s\" matched to \"%s\" by bare property name",
                                            exp_name, spi_name)));
                            break;
                        }
                    }
                }

                /* If still no match, leave col_map[a] = -1 (will yield NULL) */
            }

            srfctx->rows = (HeapTuple *) palloc(nrows * sizeof(HeapTuple));

            for (int i = 0; i < nrows; i++)
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
                    {
                        Oid spi_type = TupleDescAttr(spi_tupdesc, spi_idx)->atttypid;
                        Oid exp_type = TupleDescAttr(expected_tupdesc, a)->atttypid;
                        if (spi_type == exp_type)
                        {
                            vals[a] = SPI_getbinval(spi_tup, spi_tupdesc, spi_idx + 1, &isnull);
                        }
                        else
                        {
                            /* Type mismatch: convert through text representation */
                            char *str = SPI_getvalue(spi_tup, spi_tupdesc, spi_idx + 1);
                            if (str)
                            {
                                Oid typioparam, typinput;
                                int32 typmod;
                                FmgrInfo finfo;
                                getTypeInputInfo(exp_type, &typinput, &typioparam);
                                typmod = TupleDescAttr(expected_tupdesc, a)->atttypmod;
                                fmgr_info(typinput, &finfo);
                                vals[a] = InputFunctionCall(&finfo, str, typioparam, typmod);
                                pfree(str);
                            }
                            else
                                isnull = true;
                        }
                    }
                    else
                        isnull = true;
                    nls[a] = isnull;
                }

                /* Build the heap tuple now, while SPI data is still valid */
                ht = heap_form_tuple(expected_tupdesc, vals, nls);
                srfctx->rows[i] = ht;

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

    if (srfctx->current_row < srfctx->nrows)
    {
        int i = srfctx->current_row;
        Datum result = HeapTupleGetDatum(srfctx->rows[i]);
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
/* Replication: convert a SQL row change to a standard Cypher           */
/* CREATE / MERGE / DELETE statement.                                  */
/*                                                                     */
/* The declarative entry point is ladybug.enable_replication(graph),    */
/* which creates a Postgres PUBLICATION (conventional name) and an      */
/* AFTER INSERT/UPDATE/DELETE row trigger on each registered node/rel   */
/* table.  This trigger turns the changed row into a standard Cypher    */
/* statement and records it in ladybug._replication_log (the change     */
/* stream that the ladybug postgres client consumes as its "subscription". */
/* ------------------------------------------------------------------ */

/*
 * Render a single Datum as a Cypher literal.  Numeric and boolean values
 * are emitted bare; every other value is emitted as a single-quoted Cypher
 * string with embedded quotes doubled.
 */
static char *
cypher_literal(Oid typid, Datum val)
{
    Oid         outfunc;
    bool        isvarlena;
    FmgrInfo    finfo;
    char       *raw;
    char       *copy;

    char        typcategory;
    bool        typispreferred;

    getTypeOutputInfo(typid, &outfunc, &isvarlena);
    fmgr_info(outfunc, &finfo);
    raw = OutputFunctionCall(&finfo, val);

    /* Determine the type category (PG18: get_type_category_preferred). */
    get_type_category_preferred(typid, &typcategory, &typispreferred);
    if (typcategory == TYPCATEGORY_NUMERIC ||
        typcategory == TYPCATEGORY_BOOLEAN)
    {
        copy = pstrdup(raw);
        pfree(raw);
        return copy;
    }
    else
    {
        StringInfoData b;
        const char *p;

        initStringInfo(&b);
        appendStringInfoChar(&b, '\'');
        for (p = raw; *p; p++)
        {
            if (*p == '\'')
                appendStringInfoChar(&b, '\'');
            appendStringInfoChar(&b, *p);
        }
        appendStringInfoChar(&b, '\'');
        copy = b.data;
        pfree(raw);
        return copy;
    }
}

/*
 * Fetch the named column of a row as a Cypher literal.  Uses SPI_getbinval
 * so the SPI connection must be active when this is called.  Returns the
 * string "null" when the column is NULL or does not exist.
 */
static char *
row_column_literal(HeapTuple row, TupleDesc td, const char *colname, bool *found)
{
    int         i;

    *found = false;
    for (i = 0; i < td->natts; i++)
    {
        if (strcmp(NameStr(TupleDescAttr(td, i)->attname), colname) == 0)
        {
            bool        isnull;
            Datum       d;

            *found = true;
            d = SPI_getbinval(row, td, i + 1, &isnull);
            if (isnull)
                return pstrdup("null");
            return cypher_literal(TupleDescAttr(td, i)->atttypid, d);
        }
    }
    return pstrdup("null");
}

/* Append ", col: literal" for every column of the row. */
static void
append_all_props(StringInfo s, HeapTuple row, TupleDesc td)
{
    int         i;

    for (i = 0; i < td->natts; i++)
    {
        bool        isnull;
        Datum       d;
        char       *lit;

        if (i > 0)
            appendStringInfoString(s, ", ");
        d = SPI_getbinval(row, td, i + 1, &isnull);
        lit = isnull ? pstrdup("null")
                     : cypher_literal(TupleDescAttr(td, i)->atttypid, d);
        appendStringInfo(s, "%s: %s",
                         NameStr(TupleDescAttr(td, i)->attname), lit);
        pfree(lit);
    }
}

/* Append "n.col = literal, ..." (SET clause) for every column except id. */
static void
append_set_items(StringInfo s, HeapTuple row, TupleDesc td,
                 const char *idcol, const char *var)
{
    int         i;
    bool        first = true;

    for (i = 0; i < td->natts; i++)
    {
        const char *name = NameStr(TupleDescAttr(td, i)->attname);
        bool        isnull;
        Datum       d;
        char       *lit;

        if (idcol && strcmp(name, idcol) == 0)
            continue;
        d = SPI_getbinval(row, td, i + 1, &isnull);
        lit = isnull ? pstrdup("null")
                     : cypher_literal(TupleDescAttr(td, i)->atttypid, d);
        if (!first)
            appendStringInfoString(s, ", ");
        appendStringInfo(s, "%s.%s = %s", var, name, lit);
        first = false;
        pfree(lit);
    }
}

/*
 * Build the standard Cypher statement for a row change.
 *   INSERT: CREATE (n:Label {cols...})
 *   UPDATE: MERGE (n:Label {id: ..}) SET n.col = .., ..
 *   DELETE: MATCH (n:Label {id: ..}) DELETE n
 * For edge (rel) tables the endpoints are matched by their id column and
 * the relationship is created/merged between them.
 */
static char *
build_replication_cypher(HeapTuple row, TupleDesc td,
                         const char *kind, const char *label,
                         const char *idcol, const char *fromcol,
                         const char *tocol, char op)
{
    StringInfoData s;
    bool        found;

    initStringInfo(&s);

    if (strcmp(kind, "edge") == 0)
    {
        char       *f = row_column_literal(row, td, fromcol, &found);
        char       *t = row_column_literal(row, td, tocol, &found);
        char       *id = row_column_literal(row, td, idcol, &found);

        if (op == 'I')
        {
            appendStringInfoString(&s, "MATCH (a {id: ");
            appendStringInfoString(&s, f);
            appendStringInfoString(&s, "}), (b {id: ");
            appendStringInfoString(&s, t);
            appendStringInfoString(&s, "}) CREATE (a)-[r:");
            appendStringInfoString(&s, label);
            appendStringInfoString(&s, " {id: ");
            appendStringInfoString(&s, id);
            appendStringInfoString(&s, ", ");
            append_all_props(&s, row, td);
            appendStringInfoString(&s, "}]->(b)");
        }
        else if (op == 'U')
        {
            appendStringInfoString(&s, "MATCH (a {id: ");
            appendStringInfoString(&s, f);
            appendStringInfoString(&s, "}), (b {id: ");
            appendStringInfoString(&s, t);
            appendStringInfoString(&s, "}) MERGE (a)-[r:");
            appendStringInfoString(&s, label);
            appendStringInfoString(&s, " {id: ");
            appendStringInfoString(&s, id);
            appendStringInfoString(&s, "}]->(b) SET ");
            append_set_items(&s, row, td, idcol, "r");
        }
        else
        {
            appendStringInfoString(&s, "MATCH ()-[r:");
            appendStringInfoString(&s, label);
            appendStringInfoString(&s, " {id: ");
            appendStringInfoString(&s, id);
            appendStringInfoString(&s, "}]->() DELETE r");
        }
        pfree(f);
        pfree(t);
        pfree(id);
    }
    else
    {
        char       *id = row_column_literal(row, td, idcol, &found);

        if (op == 'I')
        {
            appendStringInfoString(&s, "CREATE (n:");
            appendStringInfoString(&s, label);
            appendStringInfoString(&s, " {");
            append_all_props(&s, row, td);
            appendStringInfoString(&s, "})");
        }
        else if (op == 'U')
        {
            appendStringInfoString(&s, "MERGE (n:");
            appendStringInfoString(&s, label);
            appendStringInfoString(&s, " {id: ");
            appendStringInfoString(&s, id);
            appendStringInfoString(&s, "}) SET ");
            append_set_items(&s, row, td, idcol, "n");
        }
        else
        {
            appendStringInfoString(&s, "MATCH (n:");
            appendStringInfoString(&s, label);
            appendStringInfoString(&s, " {id: ");
            appendStringInfoString(&s, id);
            appendStringInfoString(&s, "}) DELETE n");
        }
        pfree(id);
    }

    return s.data;
}

/* ------------------------------------------------------------------ */
/* ladybug_replication_trigger() -> trigger                            */
/* AFTER INSERT/UPDATE/DELETE row trigger installed by                  */
/* ladybug.enable_replication().  Converts the changed row into a       */
/* standard Cypher CREATE/MERGE/DELETE statement and records it in      */
/* ladybug._replication_log (the change stream consumed as the          */
/* "subscription" by the ladybug postgres client).                     */
/*                                                                     */
/* Trigger args: graph, kind, label, id_column[, from_col, to_col]      */
/* ------------------------------------------------------------------ */
PG_FUNCTION_INFO_V1(ladybug_replication_trigger);

Datum
ladybug_replication_trigger(PG_FUNCTION_ARGS)
{
    TriggerData *trigdata = (TriggerData *) fcinfo->context;
    int         nargs;
    char      **args;
    const char *graph;
    const char *kind;
    const char *label;
    const char *idcol;
    const char *fromcol = NULL;
    const char *tocol = NULL;
    Relation    rel;
    TupleDesc   td;
    const char *opname;
    char        op;
    HeapTuple   row;
    char       *cypher;
    char       *tblname;
    StringInfoData ins;
    int         ret;

    if (!CALLED_AS_TRIGGER(fcinfo))
        ereport(ERROR,
                (errmsg("ladybug.replication_trigger() must be called as a trigger")));
    if (!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
        ereport(ERROR,
                (errmsg("ladybug.replication_trigger() must be a row-level trigger")));

    nargs = trigdata->tg_trigger->tgnargs;
    args = trigdata->tg_trigger->tgargs;
    if (nargs < 4)
        ereport(ERROR,
                (errmsg("ladybug.replication_trigger() requires trigger args "
                        "(graph, kind, label, id_column[, from_col, to_col])")));
    graph = args[0];
    kind = args[1];
    label = args[2];
    idcol = args[3];
    if (nargs >= 6)
    {
        fromcol = args[4];
        tocol = args[5];
    }

    rel = trigdata->tg_relation;
    td = RelationGetDescr(rel);
    tblname = MemoryContextStrdup(CurrentMemoryContext,
                                  RelationGetRelationName(rel));

    /*
     * tg_trigtuple is always set for row-level triggers: the inserted row
     * (INSERT), the old row (UPDATE/DELETE).  tg_newtuple holds the new row
     * for UPDATE only (it is NULL for INSERT, even for AFTER triggers).
     * We always want the *new* values for INSERT/UPDATE.
     */
    if (TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
    {
        op = 'I';
        opname = "INSERT";
        row = trigdata->tg_trigtuple;
    }
    else if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
    {
        op = 'U';
        opname = "UPDATE";
        row = trigdata->tg_newtuple;
    }
    else
    {
        op = 'D';
        opname = "DELETE";
        row = trigdata->tg_trigtuple;
    }

    /* Connect so SPI_getbinval and the log INSERT work. */
    ret = SPI_connect();
    if (ret != SPI_OK_CONNECT)
        ereport(ERROR,
                (errmsg("ladybug: replication trigger: SPI_connect failed")));

    cypher = build_replication_cypher(row, td, kind, label, idcol,
                                      fromcol, tocol, op);

    initStringInfo(&ins);
    appendStringInfoString(&ins,
                           "INSERT INTO ladybug._replication_log "
                           "(graph, kind, label, table_name, operation, cypher) "
                           "VALUES (");
    appendStringInfoString(&ins, quote_literal_cstr(graph));
    appendStringInfoString(&ins, ", ");
    appendStringInfoString(&ins, quote_literal_cstr(kind));
    appendStringInfoString(&ins, ", ");
    appendStringInfoString(&ins, quote_literal_cstr(label));
    appendStringInfoString(&ins, ", ");
    appendStringInfoString(&ins, quote_literal_cstr(tblname));
    appendStringInfoString(&ins, ", ");
    appendStringInfoString(&ins, quote_literal_cstr(opname));
    appendStringInfoString(&ins, ", ");
    appendStringInfoString(&ins, quote_literal_cstr(cypher));
    appendStringInfoString(&ins, ")");

    ret = SPI_execute(ins.data, false, 0);
    if (ret < 0)
        ereport(WARNING,
                (errmsg("ladybug: replication trigger could not record change")));

    SPI_finish();

    /* AFTER triggers: the return value is ignored. */
    return PointerGetDatum(trigdata->tg_trigtuple);
}

/* ------------------------------------------------------------------ */
/* ladybug.replay_replication(graph) -> int                            */
/* Replay the captured change stream (ladybug._replication_log) as      */
/* Cypher statements into the embedded Ladybug engine, so the graph     */
/* data is (re)materialised in ladybug native storage.  This is the     */
/* apply side of the "subscription": it consumes the SQL->Cypher        */
/* statements and hands each to the embedded ladybug connection.  Statements      */
/* that fail (e.g. missing native node/rel table) are skipped, and the  */
/* number of successfully applied statements is returned.               */
/* ------------------------------------------------------------------ */
PG_FUNCTION_INFO_V1(ladybug_replay_replication);

Datum
ladybug_replay_replication(PG_FUNCTION_ARGS)
{
    text           *graph_text;
    char           *graph;
    const char     *err_msg = NULL;
    LadybugBridge  *b;
    int             nrows = 0;
    int             ncols = 0;
    SPITupleTable  *tuptable = NULL;
    int             applied = 0;
    int             failed = 0;
    int             i;

    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errmsg("ladybug.replay_replication(): NULL argument is not allowed")));
    graph_text = PG_GETARG_TEXT_PP(0);
    graph = text_to_cstring(graph_text);

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

    nrows = spi_execute_and_capture(
                psprintf("SELECT cypher FROM ladybug._replication_log "
                         "WHERE graph = %s ORDER BY id",
                         quote_literal_cstr(graph)),
                &tuptable, &ncols);

    if (nrows > 0 && tuptable != NULL)
    {
        for (i = 0; i < nrows; i++)
        {
            char       *cypher;
            char       *res;
            const char *e = NULL;

            cypher = SPI_getvalue(tuptable->vals[i], tuptable->tupdesc, 1);
            if (cypher == NULL)
                continue;
            res = ladybug_bridge_direct_sql(b, cypher, &e);
            if (res != NULL)
            {
                pfree(res);
                applied++;
            }
            else
            {
                failed++;
                ereport(NOTICE,
                        (errmsg("ladybug: replay skipped: %s",
                                cypher),
                         e ? errdetail("%s", e) : 0));
                if (e)
                    pfree((char *) e);
            }
            pfree(cypher);
        }
    }

    SPI_finish();
    pfree(graph);

    if (failed > 0)
        ereport(NOTICE,
                (errmsg("ladybug: replayed %d change(s), skipped %d (ensure native "
                        "ladybug node/rel tables exist)", applied, failed)));

    PG_RETURN_INT32(applied);
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
        int         printed;

        if (len == 0)
        {
            /* No DataDir available; fall back to a relative path. */
            printed = snprintf(ladybug_default_storage_path,
                               sizeof(ladybug_default_storage_path),
                               "storage.lbdb");
        }
        else if (dir[len - 1] == '/')
        {
            printed = snprintf(ladybug_default_storage_path,
                               sizeof(ladybug_default_storage_path),
                               "%sstorage.lbdb", dir);
        }
        else
        {
            printed = snprintf(ladybug_default_storage_path,
                               sizeof(ladybug_default_storage_path),
                               "%s/storage.lbdb", dir);
        }

        if (printed < 0 || (size_t)printed >= sizeof(ladybug_default_storage_path))
        {
            ereport(WARNING,
                    (errmsg("ladybug: default storage path too long, using in-memory storage"),
                     errdetail("DataDir length %zu exceeds buffer size %zu", len,
                               sizeof(ladybug_default_storage_path))));
            ladybug_default_storage_path[0] = '\0';
        }
    }

    DefineCustomStringVariable(
        "ladybug.pg_connstr",
        "Postgres connection string for ATTACHing the current database "
        "to the Ladybug engine so its planner can resolve local tables.",
        "E.g. 'host=localhost port=5432 dbname=mydb user=me'. "
        "Must be set before calling ladybug.cypher() or ladybug.pushed_sql(). "
        "Only superusers may set this GUC.",
        &ladybug_pg_connstr,
        "",
        PGC_SUSET,
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
        PGC_SUSET,
        0,
        NULL, NULL, NULL);

    elog(LOG, "pg_ladybug: loaded (Cypher via embedded Ladybug engine + native SPI), default storage='%s'",
         ladybug_default_storage_path);
}