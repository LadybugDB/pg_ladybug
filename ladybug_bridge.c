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
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"
#include "access/htup_details.h"
#include "fmgr.h"

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
typedef bool               (*pf_value_is_null)(lbug_value *);
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
    pf_value_is_null                  value_is_null;
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
/*  Forward declarations of public API                               */
/* ================================================================ */

LadybugBridge *ladybug_bridge_acquire(const char **err_msg);
char  *ladybug_bridge_direct_sql(LadybugBridge *b, const char *sql, const char **err_msg);
bool   ladybug_bridge_attach_postgres(LadybugBridge *b, const char *pg_connstr, const char **err_msg);
char  *ladybug_bridge_pushed_sql(LadybugBridge *b, const char *cypher, const char **err_msg);
char  *ladybug_bridge_explain(LadybugBridge *b, const char *cypher, const char **err_msg);
char  *ladybug_bridge_execute_query(LadybugBridge *b, const char *query, const char **err_msg);
int    ladybug_bridge_fill_tuplestore_from_query(LadybugBridge *b, const char *query, Tuplestorestate *ts, TupleDesc tupdesc, const char **err_msg);
void   ladybug_bridge_release(LadybugBridge *b);

/* ================================================================ */
/*  Internal helpers                                                 */
/* ================================================================ */

/*
 * Resolve a single symbol, set *err_msg and return false on failure.
 */
static bool
resolve_sym(void *handle, const char *name, void **out, const char **err_msg)
{
    void *sym;
    const char *dl_err;

    dlerror();                                  /* clear stale error */
    sym = dlsym(handle, name);
    dl_err = dlerror();
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
    const unsigned char *p;
    bool started;
    int last_non_space_end;

    initStringInfo(&buf);
    p = (const unsigned char *) line;
    started = false;
    last_non_space_end = 0;             /* tracked for right-trim */

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
 * Extract the pushed-down SQL from an EXPLAIN plan text.
 *
 * Strategy:
 * 1. Look for "Function:" section (from ForeignJoinPushDownOptimizer).
 *    If found, capture text after it (the full SQL pushed down).
 * 2. If no "Function:" section, construct a SELECT query from the
 *    SCAN_NODE_TABLE (table name, properties) and ORDER_BY sections.
 *
 * Returns a palloc'd SQL string, or NULL on failure.
 */
static char *
extract_pushed_sql(const char *plan_text)
{
    if (plan_text == NULL || plan_text[0] == '\0')
        return NULL;

    /* First pass: try to find a "Function:" section (foreign pushdown) */
    {
        StringInfoData result;
        bool capturing;
        bool first_part;
        bool found_function;
        const char *line_start;

        initStringInfo(&result);
        capturing = false;
        first_part = true;
        found_function = false;
        line_start = plan_text;
        while (line_start && *line_start)
        {
            const char *line_end = strchr(line_start, '\n');
            size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
            char *line = pnstrdup(line_start, line_len);

            if (!capturing && strstr(line, "Function:") != NULL)
            {
                char *after;
                char *cleaned;

                found_function = true;
                after = strstr(line, "Function:");
                after += strlen("Function:");
                cleaned = clean_plan_line(after);
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
                char *cleaned;

                if (strstr(line, "Expressions:") != NULL ||
                    strstr(line, "NumOutputTuples:") != NULL)
                {
                    pfree(line);
                    break;
                }
                cleaned = clean_plan_line(line);
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

        if (found_function && result.len > 0)
        {
            return result.data;
        }
        if (found_function)
        {
            pfree(result.data);
            return NULL;
        }
        pfree(result.data);
    }

    /*
     * Second pass: no "Function:" section found.  Construct a simple
     * SELECT * FROM <table> [WHERE <conditions>] [ORDER BY ...].
     * Column ordering is handled by name-based mapping in pg_ladybug.c.
     */
    {
        StringInfoData from_table;
        StringInfoData where_clause;
        StringInfoData order_by;
        bool order_first;
        bool got_order_by;
        bool in_filter;
        StringInfoData filter_text;
        const char *line_start;

        initStringInfo(&from_table);
        initStringInfo(&where_clause);
        initStringInfo(&order_by);
        order_first = true;
        got_order_by = false;
        in_filter = false;
        initStringInfo(&filter_text);
        line_start = plan_text;
        while (line_start && *line_start)
        {
            const char *line_end = strchr(line_start, '\n');
            size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
            char *line = pnstrdup(line_start, line_len);
            char *cleaned = clean_plan_line(line);

            if (cleaned[0] != '\0')
            {
                char *uc = cleaned;

                /* Detect FILTER operator start */
                if (strstr(uc, "FILTER[") != NULL ||
                    strstr(uc, "FILTER [") != NULL)
                {
                    in_filter = true;
                }
                else if (in_filter)
                {
                    /* Collect filter content until we hit a known section */
                    if (strstr(uc, "NumOutputTuples:") != NULL ||
                        strstr(uc, "ExecutionTime:") != NULL ||
                        strstr(uc, "Tables:") != NULL ||
                        strstr(uc, "Order By:") != NULL ||
                        strstr(uc, "Expressions:") != NULL ||
                        strstr(uc, "SCAN_") != NULL ||
                        strstr(uc, "PROJECTION") != NULL ||
                        strstr(uc, "EXTEND") != NULL ||
                        strstr(uc, "HASH_JOIN") != NULL)
                    {
                        in_filter = false;
                    }
                    else
                    {
                        /* Accumulate filter text */
                        if (filter_text.len > 0)
                            appendStringInfoChar(&filter_text, ' ');
                        appendStringInfoString(&filter_text, uc);
                    }
                }

                /* Extract table name from "Tables: <name>" */
                if (strstr(uc, "Tables:") != NULL)
                {
                    char *t = strstr(uc, "Tables:");
                    t += strlen("Tables:");
                    while (*t == ' ') t++;
                    if (from_table.len == 0)
                        appendStringInfoString(&from_table, t);
                }

                /* Extract ORDER BY from FIRST "Order By: <expr>" */
                if (!got_order_by && strstr(uc, "Order By:") != NULL)
                {
                    char *o;
                    char *ord;
                    char *end;
                    char saved;

                    got_order_by = true;
                    o = strstr(uc, "Order By:");
                    o += strlen("Order By:");
                    while (*o == ' ') o++;
                    ord = o;
                    while (*ord)
                    {
                        char *col;
                        char *dot;

                        while (*ord == ' ') ord++;
                        if (*ord == '\0') break;
                        end = ord;
                        while (*end && *end != ',' && *end != '\n' && *end != '\r') end++;
                        saved = *end;
                        *end = '\0';
                        col = ord;
                        dot = strchr(col, '.');
                        if (dot) col = dot + 1;
                        if (!order_first)
                            appendStringInfoString(&order_by, ", ");
                        appendStringInfoString(&order_by, col);
                        order_first = false;
                        *end = saved;
                        ord = end;
                        if (*ord == ',') ord++;
                    }
                }
            }

            pfree(cleaned);
            pfree(line);
            line_start = line_end ? line_end + 1 : NULL;
        }

        /* Convert accumulated filter text to WHERE clause */
        if (filter_text.len > 0)
        {
            char *fp = filter_text.data;
            char *where_result = NULL;

            /* Map function name to SQL operator */
            const char *op = NULL;
            if (strstr(fp, "GREATER_THAN_EQUALS") || strstr(fp, "GREATER_EQUALS"))
                op = ">=";
            else if (strstr(fp, "GREATER_THAN"))
                op = ">";
            else if (strstr(fp, "LESS_THAN_EQUALS") || strstr(fp, "LESS_EQUALS"))
                op = "<=";
            else if (strstr(fp, "LESS_THAN"))
                op = "<";
            else if (strstr(fp, "NOT_EQUALS"))
                op = "<>";
            else if (strstr(fp, "EQUALS") || strstr(fp, "EQUALS_TO"))
                op = "=";

            if (op != NULL)
            {
                /* Extract the inner content between the outermost parens */
                char *open_paren = strchr(fp, '(');
                char *close_paren = strrchr(fp, ')');
                if (open_paren && close_paren && close_paren > open_paren)
                {
                    size_t inner_len = close_paren - open_paren - 1;
                    char *inner = pnstrdup(open_paren + 1, inner_len);

                    /* Now inner contains something like "CAST(n.age) 28" */
                    /* Remove CAST() wrappers */
                    char *clean = pstrdup(inner);
                    {
                        /* Simple approach: remove CAST(...) */
                        char *src = inner;
                        char *dst = clean;
                        while (*src)
                        {
                            if (strncmp(src, "CAST(", 5) == 0)
                            {
                                int d;

                                src += 5;
                                d = 1;
                                while (*src && d > 0)
                                {
                                    if (*src == '(') d++;
                                    if (*src == ')') d--;
                                    if (d > 0) { *dst = *src; dst++; src++; }
                                    else { src++; }
                                }
                            }
                            else
                            {
                                *dst = *src;
                                dst++;
                                src++;
                            }
                        }
                        *dst = '\0';
                    }

                    /* Now clean contains something like "n.age 28" */
                    /* Split by spaces to get left and right operands */
                    {
                        char *left_s;
                        char *mid_s;
                        char *right_s;
                        char *l;
                        char *dot;
                        size_t rl;

                        left_s = clean;
                        while (*left_s == ' ') left_s++;
                        mid_s = left_s;
                        while (*mid_s && *mid_s != ' ') mid_s++;
                        if (*mid_s == ' ')
                        {
                            *mid_s = '\0';
                            mid_s++;
                            while (*mid_s == ' ') mid_s++;
                            right_s = mid_s;
                            /* Strip trailing parens or spaces */
                            rl = strlen(right_s);
                            while (rl > 0 && (right_s[rl-1] == ')' || right_s[rl-1] == ' '))
                                right_s[--rl] = '\0';

                            /* Strip alias prefix */
                            l = left_s;
                            dot = strchr(l, '.');
                            if (dot) l = dot + 1;

                            where_result = psprintf("%s %s %s", l, op, right_s);
                        }
                    }
                    pfree(inner);
                    pfree(clean);
                }
            }

            if (where_result != NULL)
            {
                appendStringInfoString(&where_clause, where_result);
                pfree(where_result);
            }
            else
            {
                /* Fallback: use raw filter text */
                appendStringInfoString(&where_clause, fp);
            }
        }

        pfree(filter_text.data);

        if (from_table.len == 0)
        {
            pfree(from_table.data);
            pfree(where_clause.data);
            pfree(order_by.data);
            return NULL;
        }

        {
            StringInfoData sql;
            initStringInfo(&sql);
            appendStringInfoString(&sql, "SELECT * FROM ");
            appendStringInfoString(&sql, from_table.data);
            if (where_clause.len > 0)
            {
                appendStringInfoString(&sql, " WHERE ");
                appendStringInfoString(&sql, where_clause.data);
            }
            if (order_by.len > 0)
            {
                appendStringInfoString(&sql, " ORDER BY ");
                appendStringInfoString(&sql, order_by.data);
            }

            pfree(from_table.data);
            pfree(where_clause.data);
            pfree(order_by.data);

            return sql.data;
        }
    }
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
    const char *lib_path;
    void *handle;
    const char *e;
    lbug_system_config cfg;
    lbug_state st;

    if (bridge.inited)
        return &bridge;

    lib_path = GetConfigOptionByName("ladybug.lib_path", NULL, false);
    if (lib_path == NULL || lib_path[0] == '\0')
        lib_path = "liblbug.so";

    dlerror();
    handle = dlopen(lib_path, RTLD_NOW | RTLD_GLOBAL);
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
    e = NULL;
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
    RESOLVE(value_is_null,                 pf_value_is_null,                    "lbug_value_is_null");
    RESOLVE(value_to_string,               pf_value_to_string,                  "lbug_value_to_string");
    RESOLVE(query_result_get_num_columns,  pf_query_result_get_num_columns,     "lbug_query_result_get_num_columns");
    RESOLVE(query_result_get_num_tuples,   pf_query_result_get_num_tuples,     "lbug_query_result_get_num_tuples");
    RESOLVE(query_result_reset_iterator,   pf_query_result_reset_iterator,     "lbug_query_result_reset_iterator");
    RESOLVE(query_result_destroy,          pf_query_result_destroy,             "lbug_query_result_destroy");
    RESOLVE(destroy_string,                pf_destroy_string,                   "lbug_destroy_string");

    #undef RESOLVE

    /* Create in-memory Ladybug database + connection. */
    cfg = bridge.default_system_config();
    st = bridge.database_init(":memory:", cfg, &bridge.database);
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

    st = b->connection_query(&b->connection, sql, &result);
    if (st != 0 || !b->query_result_is_success(&result))
    {
        lbug_err = NULL;
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

    raw = b->query_result_to_string(&result);
    copy = NULL;
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
    const char *e;

    if (b == NULL)
    {
        if (err_msg) *err_msg = pstrdup("ladybug: bridge not initialized");
        return false;
    }
    if (b->attached)
        return true;

    e = NULL;

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
    StringInfoData explain_sql;
    const char *e;
    char *plan_text;
    char *sql;

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

    sql = extract_pushed_sql(plan_text);
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
 * Release the bridge: destroy connection, database, dlclose.
 * Called only if you want to teardown the static bridge (normally we keep
 * it alive for the life of the backend).
 */
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

    st = b->connection_query(&b->connection, query, &result);
    if (st != 0 || !b->query_result_is_success(&result))
    {
        lbug_err = NULL;
        if (b->query_result_get_error_message)
            lbug_err = b->query_result_get_error_message(&result);
        if (err_msg)
            *err_msg = psprintf("ladybug: query failed: %s",
                                lbug_err ? lbug_err : "(no error message)");
        if (lbug_err)
            b->destroy_string(lbug_err);
        b->query_result_destroy(&result);
        return -1;
    }

    /* Get number of columns */
    num_cols = (int)b->query_result_get_num_columns(&result);
    natts = tupdesc->natts;

    if (num_cols != natts)
    {
        if (err_msg)
            *err_msg = psprintf("ladybug: column count mismatch: query returns %d columns, expected %d",
                                num_cols, natts);
        b->query_result_destroy(&result);
        return -1;
    }

    /* Iterate through result tuples */
    nrows = 0;
    while (b->query_result_has_next(&result))
    {
        lbug_flat_tuple flat_tuple;
        Datum *values;
        bool  *nulls;
        HeapTuple tuple;

        memset(&flat_tuple, 0, sizeof(flat_tuple));

        st = b->query_result_get_next(&result, &flat_tuple);
        if (st != 0)
        {
            if (err_msg)
                *err_msg = pstrdup("ladybug: error fetching next tuple");
            b->query_result_destroy(&result);
            return -1;
        }

        values = (Datum *) palloc0(natts * sizeof(Datum));
        nulls  = (bool  *) palloc0(natts * sizeof(bool));

        for (int a = 0; a < natts; a++)
        {
            lbug_value val;
            char *str;

            memset(&val, 0, sizeof(val));

            st = b->flat_tuple_get_value(&flat_tuple, (uint64_t)a, &val);
            if (st != 0 || b->value_is_null(&val))
            {
                nulls[a] = true;
                values[a] = (Datum) 0;
                continue;
            }

            /* Get the value as a string */
            str = NULL;
            st = b->value_get_string(&val, &str);
            if (st != 0 || str == NULL)
            {
                /* Fall back to to_string */
                str = b->value_to_string(&val);
            }

            if (str != NULL)
            {
                /* Convert string to PG datum using the expected type's input function */
                Oid typid;
                int32 typmod;
                Oid typioparam;
                Oid typinput;
                FmgrInfo finfo;

                typid = TupleDescAttr(tupdesc, a)->atttypid;
                typmod = TupleDescAttr(tupdesc, a)->atttypmod;
                getTypeInputInfo(typid, &typinput, &typioparam);
                fmgr_info(typinput, &finfo);

                values[a] = InputFunctionCall(&finfo, str, typioparam, typmod);
                nulls[a] = false;

                b->destroy_string(str);
            }
            else
            {
                nulls[a] = true;
                values[a] = (Datum) 0;
            }
        }

        /* Build and store the tuple */
        tuple = heap_form_tuple(tupdesc, values, nulls);
        tuplestore_puttuple(ts, tuple);
        heap_freetuple(tuple);

        pfree(values);
        pfree(nulls);

        nrows++;
    }

    b->query_result_destroy(&result);
    return nrows;
}

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