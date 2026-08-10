/*
 * ladybug_bridge_guard.cpp
 *
 * C++ exception boundary for the Ladybug bridge.
 *
 * liblbug is a C++ library exposed through a C API (lib/lbug.h).  Its C
 * API is not noexcept in practice: certain engine paths (planner,
 * executor, catalog reopens) can throw C++ exceptions -- e.g.
 * std::out_of_range from an internal unordered_map::at -- and the C API
 * does not always translate those into lbug_state returns.  When such an
 * exception escapes into the PostgreSQL backend, the C++ runtime calls
 * std::terminate() -> abort(), which the postmaster treats as a backend
 * crash and recovers from by killing every other session (SIGABRT /
 * cluster-wide restart).  See GitHub issue #2.
 *
 * This translation unit is compiled as C++ (PGXS builds *.cpp with
 * $(CXX)).  It provides extern "C" guard wrappers for the liblbug calls
 * the bridge invokes on the executing/planning paths.  Each guard runs
 * the real liblbug call inside a try/catch(...); on an escaped exception
 * it records a human-readable message through an out-parameter (gerr,
 * palloc'd, owned by the caller) and returns LbugError, so the bridge can
 * report the failure via ereport()/NOTICE the way it already does for
 * ordinary liblbug errors -- instead of letting the exception unwind
 * into Postgres.
 *
 * PostgreSQL uses setjmp/longjmp for ereport(ERROR); longjmp does not
 * run C++ destructors or catch handlers, so these guards only ever catch
 * genuine C++ exceptions thrown by liblbug.  They never swallow a PG
 * ereport.
 */
/*
 * PostgreSQL's C headers are not guarded by PG_BEGIN_DECLS/extern "C" in
 * this build, so when compiled as C++ every PG function declaration
 * (psprintf, pstrdup, pfree, ...) would get C++ (mangled) linkage and fail
 * to resolve against the C-built postgres at link/load time.  Parse all PG
 * headers under extern "C" to keep C linkage.  lbug.h already uses
 * extern "C" itself, so it is unaffected by the wrapping.
 */
extern "C" {
#include "postgres.h"
}

#include "lib/lbug.h"

#include <exception>
#include <stdexcept>
#include <string.h>

/*
 * Record an escaped-exception message.  Returns LbugError so the guard
 * can `return` it uniformly.  The message is palloc'd in the current
 * memory context; the caller frees it.
 */
static lbug_state
ladybug_guard_record_exception(const char **gerr, const char *what)
{
    if (gerr)
        *gerr = psprintf("ladybug: liblbug threw an uncaught C++ exception: %s",
                         what ? what : "(unknown)");
    return LbugError;
}

extern "C" {

/*
 * Guard lbug_database_init().  On an escaped exception the database
 * handle is left untouched (liblbug did not return a valid one); the
 * caller's existing st != 0 fallback path applies.
 */
lbug_state
ladybug_guard_database_init(const char *path, lbug_system_config cfg,
                            lbug_database *out, const char **gerr)
{
    try
    {
        return lbug_database_init(path, cfg, out);
    }
    catch (const std::exception &e)
    {
        return ladybug_guard_record_exception(gerr, e.what());
    }
    catch (...)
    {
        return ladybug_guard_record_exception(gerr, "(non-std exception)");
    }
}

/*
 * Guard lbug_connection_init().  Same contract as database_init.
 */
lbug_state
ladybug_guard_connection_init(lbug_database *db, lbug_connection *out,
                              const char **gerr)
{
    try
    {
        return lbug_connection_init(db, out);
    }
    catch (const std::exception &e)
    {
        return ladybug_guard_record_exception(gerr, e.what());
    }
    catch (...)
    {
        return ladybug_guard_record_exception(gerr, "(non-std exception)");
    }
}

/*
 * Guard lbug_connection_query() -- the primary crash site from issue #2.
 * On an escaped exception the query result may be partially initialised
 * by liblbug internals; we best-effort destroy and zero it so the caller
 * never sees a half-built result handle, then report the exception.
 */
lbug_state
ladybug_guard_connection_query(lbug_connection *conn, const char *query,
                               lbug_query_result *out, const char **gerr)
{
    try
    {
        return lbug_connection_query(conn, query, out);
    }
    catch (const std::exception &e)
    {
        try { lbug_query_result_destroy(out); } catch (...) {}
        memset(out, 0, sizeof(*out));
        return ladybug_guard_record_exception(gerr, e.what());
    }
    catch (...)
    {
        try { lbug_query_result_destroy(out); } catch (...) {}
        memset(out, 0, sizeof(*out));
        return ladybug_guard_record_exception(gerr, "(non-std exception)");
    }
}

/*
 * Guard lbug_connection_get_pushed_sql().  On an escaped exception, if
 * liblbug had already written *out_sql before throwing, free it and
 * clear it so the caller does not dereference or double-free a stale
 * pointer.
 */
lbug_state
ladybug_guard_connection_get_pushed_sql(lbug_connection *conn,
                                        const char *cypher, char **out_sql,
                                        const char **gerr)
{
    try
    {
        return lbug_connection_get_pushed_sql(conn, cypher, out_sql);
    }
    catch (const std::exception &e)
    {
        if (out_sql && *out_sql)
        {
            try { lbug_destroy_string(*out_sql); } catch (...) {}
            *out_sql = NULL;
        }
        return ladybug_guard_record_exception(gerr, e.what());
    }
    catch (...)
    {
        if (out_sql && *out_sql)
        {
            try { lbug_destroy_string(*out_sql); } catch (...) {}
            *out_sql = NULL;
        }
        return ladybug_guard_record_exception(gerr, "(non-std exception)");
    }
}

} /* extern "C" */
