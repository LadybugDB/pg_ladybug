/*
 * ladybug_bridge.c
 *
 * Runtime bridge to the embedded Ladybug graph engine (liblbug) via dlopen.
 *
 * pg_ladybug does NOT link liblbug at build time.  Instead, at first use it
 * dlopen()s the shared library (path from the GUC ladybug.lib_path), resolves
 * the C API symbols with dlsym(), creates an in-memory Ladybug database +
 * connection, and uses the planner to translate Cypher into a pushed-down SQL
 * string.  That SQL is then executed natively in the PostgreSQL backend via
 * SPI by pg_ladybug.c.
 *
 * DuckDB never loads inside the PostgreSQL backend.  Ladybug is used purely
 * as a Cypher→SQL compiler.
 *
 * The C API surface we bind is the stable ABI documented in
 * ladybug/src/include/c_api/lbug.h.  We redeclare the opaque handle types
 * locally (each is a struct with a single void* member; for query_result,
 * flat_tuple, and value there is also a bool _is_owned_by_cpp member) and do
 * NOT include lbug.h.
 */

#include "postgres.h"

#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>

#include "utils/guc.h"
#include "utils/builtins.h"

/* ================================================================ */
/*  Opaque handle types (mirror lbug.h — never include the header)  */
/* ================================================================ */

typedef int lbug_state;          /* 0 = success, 1 = error */

typedef struct {
    void *_database;
} lbug_database;

typedef struct {
    void *_connection;
} lbug_connection;

typedef struct {
    void    *_query_result;
    bool     _is_owned_by_cpp;
} lbug_query_result;

typedef struct {
    void    *_flat_tuple;
    bool     _is_owned_by_cpp;
} lbug_flat_tuple;

typedef struct {
    void    *_value;
    bool     _is_owned_by_cpp;
} lbug_value;

/*
 * lbug_system_config — passed by value.
 * Member order must match lbug.h.  On non-Apple platforms the struct ends
 * after enable_default_hash_index.  On Apple there is a trailing
 * uint32_t thread_qos.  To be ABI-safe we never hand-construct this struct;
 * we always call lbug_default_system_config() and pass its return value
 * straight into lbug_database_init().  We declare a generous struct with
 * the Apple field backfilled so the by-value call is large enough on every
 * platform; the extra trailing field is harmlessly zeroed by the default
 * config call.
 */
typedef struct {
    uint64_t buffer_pool_size;
    uint64_t max_num_threads;
    bool     enable_compression;
    bool     read_only;
    uint64_t max_db_size;
    bool     auto_checkpoint;
    uint64_t checkpoint_threshold;
    bool     throw_on_wal_replay_failure;
    bool     enable_checksums;
    bool     enable_multi_writes;
    bool     enable_default_hash_index;
    uint32_t thread_qos;            /* Apple only; zero elsewhere */
} lbug_system_config;

/* ================================================================ */
/*  Function pointer typedefs                                        */
/* ================================================================ */

typedef lbug_system_config (*pf_default_system_config)(void);
typedef lbug_state         (*pf_database_init)(const char *, lbug_system_config, lbug_database *);
typedef void               (*pf_database_destroy)(lbug_database *);
typedef lbug_state         (*pf_connection_init)(lbug_database *, lbug_connection *);
typedef void               (*pf_connection_destroy)(lbug_connection *);
typedef lbug_state         (*pf_connection_query)(lbug_connection *, const char *, lbug_query_result *);
typedef bool               (*pf_query_result_is_success)(lbug_query_result *);
typedef char *             (*pf_query_result_get_error_message)(lbug_query_result *);
typedef char *             (*pf_query_result_to_string)(lbug_query_result *);
typedef bool               (*pf_query_result_has_next)(lbug_query_result *);
typedef lbug_state         (*pf_query_result_get_next)(lbug_query_result *, lbug_flat_tuple *);
typedef lbug_state         (*pf_flat_tuple_get_value)(lbug_flat_tuple *, uint64_t, lbug_value *);
typedef lbug_state         (*pf_value_get_string)(lbug_value *, char **);
typedef char *             (*pf_value_to_string)(lbug_value *);
typedef uint64_t           (*pf_query_result_get_num_columns)(lbug_query_result *);
typedef uint64_t           (*pf_query_result_get_num_tuples)(lbug_query_result *);
typedef void               (*pf_query_result_reset_iterator)(lbug_query_result *);
typedef void               (*pf_query_result_destroy)(lbug_query_result *);
typedef void               (*pf_destroy_string)(char *);

/* ================================================================ */
/*  LadybugBridge: cached dlopen + resolved symbols + live conn      */
/* ================================================================ */

typedef struct LadybugBridge
{
    void                  *dl_handle;     /* dlopen handle          */
    lbug_database          database;       /* in-memory Ladybug DB   */
    lbug_connection        connection;    /* live connection        */
    bool                   attached;      /* ATTACH done this backend */
    bool                   inited;         /* database+conn created  */

    /* resolved symbols */
    pf_default_system_config          default_system_config;
    pf_database_init                  database_init;
    pf_database_destroy               database_destroy;
    pf_connection_init                connection_init;
    pf_connection_destroy             connection_destroy;
    pf_connection_query               connection_query;
    pf_query_result_is_success        query_result_is_success;
    pf_query_result_get_error_message query_result_get_error_message;
    pf_query_result_to_string         query_result_to_string;
    pf_query_result_has_next          query_result_has_next;
    pf_query_result_get_next          query_result_get_next;
    pf_flat_tuple_get_value           flat_tuple_get_value;
    pf_value_get_string               value_get_string;
    pf_value_to_string                value_to_string;
    pf_query_result_get_num_columns   query_result_get_num_columns;
    pf_query_result_get_num_tuples    query_result_get_num_tuples;
    pf_query_result_reset_iterator    query_result_reset_iterator;
    pf_query_result_destroy           query_result_destroy;
    pf_destroy_string                 destroy_string;
} LadybugBridge;

/* One bridge per backend (cached after first successful dlopen). */
static LadybugBridge bridge = {0};

/* GUCs — defined in pg_ladybug.c via DefineCustomStringVariable; we just
 * read them via GetConfigOptionByName().  We do not define them here to
 * avoid duplicate definitions.  The GUC names are:
 *   ladybug.lib_path    (default "liblbug.so")
 *   ladybug.pg_connstr  (default "")
 */

/* ================================================================ */
/*  Internal helpers                                                 */
/* ================================================================ */

/*
 * Resolve a single symbol, set *err_msg and return false on failure.
 */
static bool
resolve_sym(void *handle, const char *name, void **out, const char **err_msg)
{
    dlerror();                                  /* clear stale error */
    void *sym = dlsym(handle, name);
    const char *dl_err = dlerror();
    if (dl_err != NULL || sym == NULL)
    {
        if (err_msg)
            *err_msg = psprintf("ladybug: cannot resolve symbol '%s' in liblbug: %s",
                                 name, dl_err ? dl_err : "(unknown)");
        return false;
    }
    *out = sym;
    return true;
}

/*
 * Strip the box-drawing character │ (U+2502, UTF-8 0xE2 0x94 0x82) from
 * a (possibly wide) line, plus leading/trailing whitespace.  Works on a
 * single line at a time.  Returns a freshly palloc'd string.
 */
static char *
clean_plan_line(const char *line)
{
    /*
     * The box char │ is 3 bytes in UTF-8: 0xE2 0x94 0x82.  We scan the raw
     * bytes, skipping that sequence when found, and also skipping leading
     * / trailing ASCII whitespace.  The result is a compact copy.
     */
    StringInfoData buf;
    initStringInfo(&buf);

    const unsigned char *p = (const unsigned char *) line;
    bool started = false;
    int last_non_space_end = 0;             /* tracked for right-trim */

    while (*p)
    {
        if (p[0] == 0xE2 && p[1] == 0x94 && p[2] == 0x82)
        {
            p += 3;
            continue;
        }
        if (!started && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        {
            p++;
            continue;                       /* skip leading whitespace */
        }
        started = true;
        appendStringInfoCharMacro(&buf, (char) *p);
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
            last_non_space_end = buf.len;
        p++;
    }

    /* right-trim */
    if (last_non_space_end < buf.len)
        buf.data[last_non_space_end] = '\0';

    return buf.data;
}

/*
 * Extract the pushed-down SQL from an EXPLAIN plan text, porting the
 * notebook algorithm verbatim:
 *
 *   capturing = False
 *   for line in plan.splitlines():
 *       if "Function:" in line:
 *           capturing = True
 *           line = line.split("Function:", 1)[1]
 *       elif capturing and ("Expressions:" in line or "NumOutputTuples:" in line):
 *           break
 *       if capturing:
 *           cleaned = line.replace("│","").strip()
 *           if cleaned: sql_parts.append(cleaned)
 *   pushed_down_sql = " ".join(sql_parts)
 *
 * Returns a palloc'd SQL string, or NULL if no "Function:" pushdown box
 * was found.
 */
static char *
extract_pushed_sql(const char *plan_text)
{
    if (plan_text == NULL || plan_text[0] == '\0')
        return NULL;

    StringInfoData result;
    initStringInfo(&result);

    bool capturing = false;
    bool first_part = true;

    const char *line_start = plan_text;
    while (line_start && *line_start)
    {
        /* find end of line */
        const char *line_end = strchr(line_start, '\n');
        size_t      line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);

        /* make a NUL-terminated copy of the line */
        char *line = pnstrdup(line_start, line_len);

        if (!capturing && strstr(line, "Function:") != NULL)
        {
            /* start capturing; take substring after "Function:" */
            char *after = strstr(line, "Function:");
            after += strlen("Function:");
            char *cleaned = clean_plan_line(after);
            if (cleaned[0] != '\0')
            {
                if (!first_part)
                    appendStringInfoChar(&result, ' ');
                appendStringInfoString(&result, cleaned);
                first_part = false;
            }
            pfree(cleaned);
            capturing = true;
        }
        else if (capturing)
        {
            if (strstr(line, "Expressions:") != NULL ||
                strstr(line, "NumOutputTuples:") != NULL)
            {
                pfree(line);
                break;
            }
            char *cleaned = clean_plan_line(line);
            if (cleaned[0] != '\0')
            {
                if (!first_part)
                    appendStringInfoChar(&result, ' ');
                appendStringInfoString(&result, cleaned);
                first_part = false;
            }
            pfree(cleaned);
        }

        pfree(line);
        line_start = line_end ? line_end + 1 : NULL;
    }

    if (!capturing)
    {
        pfree(result.data);
        return NULL;
    }

    if (result.len == 0)
    {
        pfree(result.data);
        return NULL;
    }

    return result.data;
}

/* ================================================================ */
/*  Public bridge API (used by pg_ladybug.c)                         */
/* ================================================================ */

/*
 * ladybug_bridge_acquire — dlopen liblbug, resolve symbols, create
 * in-memory database + connection.  Returns a pointer to the static
 * bridge, or NULL on failure (with *err_msg set).  Cached after first
 * success.
 */
LadybugBridge *
ladybug_bridge_acquire(const char **err_msg)
{
    if (bridge.inited)
        return &bridge;

    const char *lib_path = GetConfigOptionByName("ladybug.lib_path", NULL, false);
    if (lib_path == NULL || lib_path[0] == '\0')
        lib_path = "liblbug.so";

    dlerror();
    void *handle = dlopen(lib_path, RTLD_NOW | RTLD_GLOBAL);
    if (handle == NULL)
    {
        const char *dl_err = dlerror();
        if (err_msg)
            *err_msg = psprintf(
                "ladybug: cannot dlopen liblbug at '%s': %s. "
                "Set ladybug.lib_path to the full path to liblbug.so/liblbug.dylib.",
                lib_path, dl_err ? dl_err : "(unknown)");
        return NULL;
    }

    bridge.dl_handle = handle;

    /* Resolve all symbols up front so failures are surfaced eagerly. */
    const char *e = NULL;
    #define RESOLVE(field, type, name) \
        do { \
            if (!resolve_sym(handle, name, (void **)&bridge.field, &e)) { \
                if (err_msg) *err_msg = e; \
                dlclose(handle); \
                memset(&bridge, 0, sizeof(bridge)); \
                return NULL; \
            } \
        } while (0)

    RESOLVE(default_system_config,          pf_default_system_config,           "lbug_default_system_config");
    RESOLVE(database_init,                 pf_database_init,                   "lbug_database_init");
    RESOLVE(database_destroy,              pf_database_destroy,                "lbug_database_destroy");
    RESOLVE(connection_init,               pf_connection_init,                 "lbug_connection_init");
    RESOLVE(connection_destroy,            pf_connection_destroy,              "lbug_connection_destroy");
    RESOLVE(connection_query,              pf_connection_query,                "lbug_connection_query");
    RESOLVE(query_result_is_success,       pf_query_result_is_success,         "lbug_query_result_is_success");
    RESOLVE(query_result_get_error_message,pf_query_result_get_error_message,  "lbug_query_result_get_error_message");
    RESOLVE(query_result_to_string,        pf_query_result_to_string,          "lbug_query_result_to_string");
    RESOLVE(query_result_has_next,         pf_query_result_has_next,           "lbug_query_result_has_next");
    RESOLVE(query_result_get_next,         pf_query_result_get_next,           "lbug_query_result_get_next");
    RESOLVE(flat_tuple_get_value,          pf_flat_tuple_get_value,             "lbug_flat_tuple_get_value");
    RESOLVE(value_get_string,              pf_value_get_string,                 "lbug_value_get_string");
    RESOLVE(value_to_string,               pf_value_to_string,                  "lbug_value_to_string");
    RESOLVE(query_result_get_num_columns,  pf_query_result_get_num_columns,     "lbug_query_result_get_num_columns");
    RESOLVE(query_result_get_num_tuples,   pf_query_result_get_num_tuples,     "lbug_query_result_get_num_tuples");
    RESOLVE(query_result_reset_iterator,   pf_query_result_reset_iterator,     "lbug_query_result_reset_iterator");
    RESOLVE(query_result_destroy,          pf_query_result_destroy,             "lbug_query_result_destroy");
    RESOLVE(destroy_string,                pf_destroy_string,                   "lbug_destroy_string");

    #undef RESOLVE

    /* Create in-memory Ladybug database + connection. */
    lbug_system_config cfg = bridge.default_system_config();
    lbug_state st = bridge.database_init(":memory:", cfg, &bridge.database);
    if (st != 0)
    {
        if (err_msg)
            *err_msg = psprintf("ladybug: lbug_database_init failed (state=%d)", (int)st);
        dlclose(handle);
        memset(&bridge, 0, sizeof(bridge));
        return NULL;
    }

    st = bridge.connection_init(&bridge.database, &bridge.connection);
    if (st != 0)
    {
        if (err_msg)
            *err_msg = psprintf("ladybug: lbug_connection_init failed (state=%d)", (int)st);
        bridge.database_destroy(&bridge.database);
        dlclose(handle);
        memset(&bridge, 0, sizeof(bridge));
        return NULL;
    }

    bridge.inited = true;
    bridge.attached = false;
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
    if (b == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: bridge not initialized");
        return NULL;
    }

    lbug_query_result result;
    memset(&result, 0, sizeof(result));

    lbug_state st = b->connection_query(&b->connection, sql, &result);
    if (st != 0 || !b->query_result_is_success(&result))
    {
        char *lbug_err = NULL;
        if (b->query_result_get_error_message)
            lbug_err = b->query_result_get_error_message(&result);
        if (err_msg)
            *err_msg = psprintf("ladybug: query failed: %s",
                                lbug_err ? lbug_err : "(no error message)");
        if (lbug_err)
            b->destroy_string(lbug_err);
        b->query_result_destroy(&result);
        return NULL;
    }

    char *raw = b->query_result_to_string(&result);
    char *copy = NULL;
    if (raw)
    {
        copy = pstrdup(raw);
        b->destroy_string(raw);
    }
    b->query_result_destroy(&result);
    return copy;
}

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
    if (b == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: bridge not initialized");
        return false;
    }
    if (b->attached)
        return true;

    const char *e = NULL;

    /* INSTALL postgres; — may already be installed */
    if (!ladybug_bridge_direct_sql(b, "INSTALL postgres;", &e))
    {
        /* non-fatal if it says "already" */
        if (e == NULL || strstr(e, "already") == NULL)
        {
            if (err_msg) *err_msg = e;
            else if (e) pfree((char *)e);
            return false;
        }
        if (e) { pfree((char *)e); e = NULL; }
    }

    /* LOAD postgres; */
    if (!ladybug_bridge_direct_sql(b, "LOAD postgres;", &e))
    {
        if (e == NULL || strstr(e, "already") == NULL)
        {
            if (err_msg) *err_msg = e;
            else if (e) pfree((char *)e);
            return false;
        }
        if (e) { pfree((char *)e); e = NULL; }
    }

    /* ATTACH '<connstr>' AS pg (dbtype POSTGRES); */
    {
        StringInfoData attach_sql;
        initStringInfo(&attach_sql);
        appendStringInfoString(&attach_sql, "ATTACH '");
        /* basic escaping of single quotes */
        for (const char *p = pg_connstr; *p; p++)
        {
            if (*p == '\'')
                appendStringInfoString(&attach_sql, "''");
            else
                appendStringInfoChar(&attach_sql, *p);
        }
        appendStringInfoString(&attach_sql, "' AS pg (dbtype POSTGRES);");

        if (!ladybug_bridge_direct_sql(b, attach_sql.data, &e))
        {
            if (e == NULL || strstr(e, "already") == NULL)
            {
                if (err_msg) *err_msg = e;
                else if (e) pfree((char *)e);
                pfree(attach_sql.data);
                return false;
            }
            if (e) { pfree((char *)e); e = NULL; }
        }
        pfree(attach_sql.data);
    }

    b->attached = true;
    return true;
}

/*
 * Run EXPLAIN <cypher> through Ladybug and extract the pushed-down SQL.
 * Returns a palloc'd SQL string, or NULL if no pushdown was possible
 * (with *err_msg set to a descriptive message).
 */
char *
ladybug_bridge_pushed_sql(LadybugBridge *b, const char *cypher, const char **err_msg)
{
    if (b == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: bridge not initialized");
        return NULL;
    }

    StringInfoData explain_sql;
    initStringInfo(&explain_sql);
    appendStringInfoString(&explain_sql, "EXPLAIN ");
    appendStringInfoString(&explain_sql, cypher);

    const char *e = NULL;
    char *plan_text = ladybug_bridge_direct_sql(b, explain_sql.data, &e);
    pfree(explain_sql.data);

    if (plan_text == NULL)
    {
        if (err_msg) *err_msg = e;
        else if (e) pfree((char *)e);
        return NULL;
    }

    char *sql = extract_pushed_sql(plan_text);
    pfree(plan_text);

    if (sql == NULL)
    {
        if (err_msg)
            *err_msg = pstrdup("ladybug: no pushed-down SQL found in EXPLAIN plan "
                                "(pattern may not be fully pushable). "
                                "Use ladybug.explain() for the full plan, or "
                                "ladybug.pushed_sql() to inspect extraction.");
        return NULL;
    }

    return sql;
}

/*
 * Run EXPLAIN <cypher> and return the full plan text (palloc'd).
 * Exposes the raw Ladybug plan for debugging.
 */
char *
ladybug_bridge_explain(LadybugBridge *b, const char *cypher, const char **err_msg)
{
    if (b == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: bridge not initialized");
        return NULL;
    }

    StringInfoData explain_sql;
    initStringInfo(&explain_sql);
    appendStringInfoString(&explain_sql, "EXPLAIN ");
    appendStringInfoString(&explain_sql, cypher);

    const char *e = NULL;
    char *plan_text = ladybug_bridge_direct_sql(b, explain_sql.data, &e);
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
 * Release the bridge: destroy connection, database, dlclose.
 * Called only if you want to teardown the static bridge (normally we keep
 * it alive for the life of the backend).
 */
void
ladybug_bridge_release(LadybugBridge *b)
{
    if (b == NULL || !b->inited)
        return;

    b->connection_destroy(&b->connection);
    b->database_destroy(&b->database);

    if (b->dl_handle)
        dlclose(b->dl_handle);

    memset(&bridge, 0, sizeof(bridge));
}