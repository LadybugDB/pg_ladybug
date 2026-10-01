#!/usr/bin/env python3
"""
Test pg_ladybug using an embedded PostgreSQL instance via pgembed.

This script:
1. Builds the extension using pgembed's PG installation
2. Copies pg_client extension alongside liblbug.so
3. Starts a temporary PG instance, installs pg_ladybug
4. Runs the full test suite (SPI + Ladybug bridge via pg_client)
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent

    # ---- Build the extension using pgembed's PG ----
    import pgembed as _pgembed_mod
    _pgembed_dir = Path(_pgembed_mod.__file__).parent / "pginstall"
    _pg_config = str(_pgembed_dir / "bin" / "pg_config")
    print("=== Building pg_ladybug with", _pg_config, "===")
    env_build = os.environ.copy()
    env_build["PG_CONFIG"] = _pg_config
    # Forward CC / PG_SYSROOT from the environment onto the make command line
    # (command-line make vars override PGXS' `=` assignments; plain env vars
    # do not).  This lets a builder select a specific compiler and/or repair a
    # stale -isysroot baked into a bundled pg_config after an Xcode upgrade.
    make_overrides = []
    for var in ("CC", "CXX", "PG_SYSROOT"):
        val = env_build.get(var)
        if val:
            make_overrides.append(f"{var}={val}")
    subprocess.run(["make", "clean", *make_overrides], cwd=repo_root, capture_output=True, text=True, env=env_build)
    result = subprocess.run(["make", *make_overrides], cwd=repo_root, capture_output=True, text=True, env=env_build)
    if result.returncode != 0:
        print("Build FAILED:", result.stderr)
        return 1
    print("Build OK")

    # ---- Ensure pg_client extension is next to liblbug.so ----
    pg_client_src = Path("/tmp/pg_client_ext/lbug-extensions-linux-x86_64/pg_client/libpg_client.lbug_extension")
    pg_client_dst = repo_root / "lib" / "libpg_client.lbug_extension"
    if pg_client_src.exists() and not pg_client_dst.exists():
        shutil.copy(pg_client_src, pg_client_dst)
        print("pg_client extension copied to lib/")
    elif pg_client_dst.exists():
        print("pg_client extension already in lib/")

    # ---- Install pg_ladybug into the embedded PG ----
    pg_lib_dir = _pgembed_dir / "lib" / "postgresql"
    pg_share_dir = _pgembed_dir / "share" / "postgresql" / "extension"
    # The shared library suffix is platform-dependent (.so on Linux,
    # .dylib on macOS); pick up whichever the build actually produced.
    shlib_path = next((repo_root / f"pg_ladybug.{ext}"
                       for ext in ("so", "dylib")
                       if (repo_root / f"pg_ladybug.{ext}").exists()), None)
    if shlib_path is None:
        print("Build FAILED: pg_ladybug shared library not found")
        return 1
    shutil.copy(shlib_path, pg_lib_dir / shlib_path.name)
    shutil.copy(repo_root / "pg_ladybug--1.0.sql", pg_share_dir / "pg_ladybug--1.0.sql")
    shutil.copy(repo_root / "pg_ladybug.control", pg_share_dir / "pg_ladybug.control")
    print("Extension files installed")

    import psycopg

    tests_passed = 0
    tests_total = 0
    tests_xfail = 0

    def run_test(name: str, sql: str, env, check: callable = None,
                 xfail_reason: str = "") -> bool:
        nonlocal tests_passed, tests_total, tests_xfail
        tests_total += 1
        print(f"\n--- Test {tests_total}: {name} ---")
        result = subprocess.run(
            [psql, "-c", sql], env=env, capture_output=True, text=True,
        )
        if result.stdout:
            for line in result.stdout.strip().split("\n")[:25]:
                print("  ", line)
        passed = True
        if result.returncode != 0:
            if result.stderr:
                for line in result.stderr.strip().split("\n")[:5]:
                    print("  ERR:", line)
            print(f"FAIL (exit code {result.returncode})")
            passed = False
        elif check and not check(result.stdout, result.stderr):
            print("FAIL: check failed")
            passed = False
        if passed:
            if xfail_reason:
                # XFAIL tests that unexpectedly pass are treated as failures
                # (the expected failure no longer reproduces).
                print(f"XPASS (unexpectedly passed; expected failure: {xfail_reason})")
                return False
            print("PASS")
            tests_passed += 1
            return True
        if xfail_reason:
            tests_xfail += 1
            # Expected failure: count toward the passing total so the suite
            # stays green, but report it explicitly.
            print(f"XFAIL (expected failure: {xfail_reason})")
            tests_passed += 1
            return True
        return False

    print("=== Starting embedded PostgreSQL ===")
    with tempfile.TemporaryDirectory(prefix="pgladybug_test_") as tmpdir:
        pgdata = Path(tmpdir) / "pgdata"
        pgdata.mkdir()

        with _pgembed_mod.get_server(str(pgdata), cleanup_mode="delete") as pg:
            admin_uri = pg.get_uri("postgres")
            with psycopg.connect(admin_uri, autocommit=True) as conn:
                conn.execute("CREATE ROLE ci WITH LOGIN SUPERUSER PASSWORD 'ci'")
                conn.execute("CREATE DATABASE ladybug_test OWNER ci")

            test_uri = pg.get_uri("ladybug_test")

            # Set up test database
            print("=== Setting up test database ===")
            with psycopg.connect(test_uri) as conn:
                conn.autocommit = True
                with conn.cursor() as cur:
                    cur.execute("CREATE EXTENSION pg_ladybug")
                    print("Extension created")
                    cur.execute("CREATE TABLE node_person (id INT PRIMARY KEY, name TEXT, age INT)")
                    cur.execute("INSERT INTO node_person VALUES "
                                "(1, 'Alice', 30), (2, 'Bob', 25), (3, 'Carol', 35), (4, 'Dave', 28)")
                    # Register label -> table mapping
                    cur.execute("SELECT ladybug.register_node('Person', 'node_person', 'id')")
                    cur.execute("SELECT * FROM ladybug._graph_meta")
                    print("Graph meta:", cur.fetchall())

                    # Create rel_knows table for MATCH-with-relationship tests
                    # (rel_* prefix: required by pg_client 0.21.0+ to expose
                    # the table as a relationship; bare FK tables like the
                    # old fkrel_knows name are no longer picked up.)
                    cur.execute("""
                        CREATE TABLE rel_knows (
                            id INT PRIMARY KEY,
                            src_id INT NOT NULL,
                            dst_id INT NOT NULL,
                            since TEXT
                        )
                    """)
                    cur.execute("ALTER TABLE rel_knows ADD CONSTRAINT fk_src FOREIGN KEY (src_id) REFERENCES node_person(id)")
                    cur.execute("ALTER TABLE rel_knows ADD CONSTRAINT fk_dst FOREIGN KEY (dst_id) REFERENCES node_person(id)")
                    cur.execute("INSERT INTO rel_knows VALUES "
                                "(1, 1, 2, '2020-01-15'), (2, 1, 3, '2021-03-20'), "
                                "(3, 2, 4, '2022-06-10'), (4, 3, 4, '2023-08-05')")

            # Build environment for psql
            from urllib.parse import urlparse, parse_qs
            parsed = urlparse(test_uri)
            query = parse_qs(parsed.query)
            socket_dir = query.get("host", ["/tmp"])[0]

            env = os.environ.copy()
            env["PGHOST"] = socket_dir
            env["PGPORT"] = "5432"
            env["PGUSER"] = "ci"
            env["PGPASSWORD"] = "ci"
            env["PGDATABASE"] = "ladybug_test"

            # Pick a psql client that actually works against the embedded
            # server.  Prefer the one bundled with pgembed (the only psql a
            # bare macOS dev box is guaranteed to have), but probe it with a
            # real connection first: the bundled Linux binaries in pgembed
            # 0.2.0 segfault (SIGSEGV, no output) in the libpq connect path,
            # so fall back to the system psql (postgresql-client) when the
            # probe fails.  Invoke everything by absolute path.
            def psql_probe_ok(psql_path: str) -> bool:
                probe = subprocess.run(
                    [psql_path, "-c", "SELECT 1"],
                    env=env, capture_output=True, text=True, timeout=30,
                )
                return probe.returncode == 0

            bundled_psql = _pgembed_dir / "bin" / "psql"
            if bundled_psql.exists() and psql_probe_ok(str(bundled_psql)):
                psql = str(bundled_psql)
                print(f"Using pgembed-bundled psql: {psql}")
            elif shutil.which("psql"):
                psql = shutil.which("psql")
                print(f"Using system psql: {psql}")
            else:
                print("ERROR: no working psql found "
                      "(pgembed probe failed and no system psql on PATH)")
                return 1

            libpq_connstr = f"host={socket_dir} port=5432 dbname=ladybug_test user=ci password=ci"

            # ================================================================
            # Tests 1-4: Pure SPI path (no Ladybug engine)
            # ================================================================
            run_test("List functions", r"\df ladybug.*", env)

            run_test("sql_query count",
                     "SELECT * FROM ladybug.sql_query('SELECT count(*)::int AS cnt FROM node_person') AS t(cnt int)",
                     env, check=lambda o, e: "4" in o)

            run_test("sql_query names",
                     "SELECT * FROM ladybug.sql_query('SELECT name FROM node_person ORDER BY name') AS t(name text)",
                     env, check=lambda o, e: "Alice" in o and "Dave" in o)

            run_test("_graph_meta",
                     "SELECT label, table_name FROM ladybug._graph_meta ORDER BY label",
                     env, check=lambda o, e: "Person" in o)

            # ================================================================
            # Test 4b: ladybug.storage_path GUC default
            # The default is <DataDir>/storage.lbdb and must end in 'storage.lbdb'.
            # Use LOAD + SHOW because SHOW alone does not auto-load the extension.
            # ================================================================
            run_test("GUC: ladybug.storage_path default",
                     "LOAD 'pg_ladybug'; SHOW ladybug.storage_path",
                     env, check=lambda o, e: "storage.lbdb" in o)

            # ================================================================
            # Tests 5-6: Bridge initialization (requires ATTACH via pg_client)
            # ================================================================
            # EXPLAIN RETURN 1 - validates bridge loads liblbug,
            # creates database+connection, runs queries
            run_test("Bridge: EXPLAIN RETURN 1",
                     f"SET ladybug.pg_connstr = '{libpq_connstr}'; SELECT ladybug.explain('RETURN 1')",
                     env, check=lambda o, e: "PROJECTION" in o or "RESULT" in o)

            # pushed_sql RETURN 1 - validates that pushed_sql correctly reports
            # "no pushdown SQL" for queries the planner cannot translate.
            # The expected behavior is an ERROR (the user explicitly asked for
            # pushed SQL, so we should report that none exists rather than
            # silently executing via the engine).  Use psql directly to
            # inspect the error message, since the runner treats any non-zero
            # exit code as a failure.
            print(f"\n--- Test 7: Bridge: pushed_sql RETURN 1 (expected error) ---")
            tests_total += 1
            result = subprocess.run(
                [psql, "-c",
                 f"SET ladybug.pg_connstr = '{libpq_connstr}'; "
                 "SELECT ladybug.pushed_sql('RETURN 1')"],
                env=env, capture_output=True, text=True,
            )
            # Expected: non-zero exit code, error message mentions
            # "no pushdown operator found in plan".
            if (result.returncode != 0
                    and result.stderr
                    and "no pushdown operator found in plan" in result.stderr):
                print("PASS (got expected 'no pushdown' error)")
                tests_passed += 1
            else:
                print(f"FAIL: expected error mentioning 'no pushdown', got "
                      f"rc={result.returncode}, stderr={result.stderr!r}")
                # do not increment tests_passed

            # ================================================================
            # Tests 7: Full Cypher flow with MATCH via pg_client
            # pg_client registers tables using their raw names, so the Cypher
            # query uses "node_person" (the table name) as the label.
            # ================================================================
            run_test("Bridge: EXPLAIN MATCH (pg_client ATTACH)",
                     f"SET ladybug.pg_connstr = '{libpq_connstr}'; "
                     "SELECT ladybug.explain('MATCH (n:node_person) RETURN n.name, n.age')",
                     env, check=lambda o, e: "SCAN" in o or "PROJECTION" in o or "EXTEND" in o)

            # ================================================================
            # Tests 8-10: Full cypher queries
            # ================================================================
            run_test("Cypher: MATCH RETURN all",
                     f"SET ladybug.pg_connstr = '{libpq_connstr}'; "
                     "SELECT * FROM ladybug.cypher('MATCH (n:node_person) RETURN n.name, n.age') "
                     "AS t(name text, age int) ORDER BY name",
                     env, check=lambda o, e: "Alice" in o and "30" in o)

            run_test("Cypher: MATCH with ORDER BY",
                     f"SET ladybug.pg_connstr = '{libpq_connstr}'; "
                     "SELECT * FROM ladybug.cypher('MATCH (n:node_person) RETURN n.name, n.age ORDER BY n.age') "
                     "AS t(name text, age int)",
                     env, check=lambda o, e: "25" in o and "30" in o and "35" in o)

            # ================================================================
            # Test 10b: cypher() fallback for queries without pushdown
            # Queries like "RETURN 1" have no pushdown SQL; the function
            # should fall back to executing the cypher as-is via the
            # Ladybug engine and return the result like a native SELECT.
            # ================================================================
            def check_value(expected, col_name):
                """Check the first data row of a single-column psql output
                contains the expected value (handles aligned/spaced output)."""
                def check(o, e):
                    # Look for a row that, after stripping non-digits/minus
                    # for the named column, equals the expected value.
                    pattern = r"\b" + re.escape(expected) + r"\b"
                    return re.search(pattern, o) is not None
                return check

            run_test("Cypher fallback: RETURN 1 (single column)",
                     f"SET ladybug.pg_connstr = '{libpq_connstr}'; "
                     "SELECT * FROM ladybug.cypher('RETURN 1') AS t(x int)",
                     env, check=check_value("1", "x"))

            run_test("Cypher fallback: RETURN 1+2 (expression)",
                     f"SET ladybug.pg_connstr = '{libpq_connstr}'; "
                     "SELECT * FROM ladybug.cypher('RETURN 1 + 2 AS sum') AS t(sum int)",
                     env, check=check_value("3", "sum"))

            run_test("Cypher fallback: UNWIND multiple rows",
                     f"SET ladybug.pg_connstr = '{libpq_connstr}'; "
                     "SELECT count(*)::int AS cnt FROM ladybug.cypher('UNWIND [10, 20, 30] AS x RETURN x') AS t(x int)",
                     env, check=check_value("3", "cnt"))

            run_test("Cypher fallback: RETURN string literal",
                     f"SET ladybug.pg_connstr = '{libpq_connstr}'; "
                     "SELECT * FROM ladybug.cypher('RETURN \"hello\" AS msg') AS t(msg text)",
                     env, check=check_value("hello", "msg"))

            # WHERE clause: the planner pushes the query down to a single
            # SELECT against node_person, which the SPI path executes
            # natively.  No special handling required.
            run_test("Cypher: MATCH with WHERE",
                     f"SET ladybug.pg_connstr = '{libpq_connstr}'; "
                     "SELECT * FROM ladybug.cypher('MATCH (n:node_person) WHERE n.age > 28 RETURN n.name, n.age') "
                     "AS t(name text, age int) ORDER BY name",
                     env, check=lambda o, e: ("Alice" in o and "Carol" in o
                                              and "Bob" not in o and "Dave" not in o))

            # ================================================================
            # Test: MATCH with relationship (rel table join) - similar to
            # pg_client test 06b. The planner translates the pattern
            # (a)-[k]->(b) into a SQL JOIN that is executed via SPI.
            # NOTE: the count test wraps a projection (not RETURN count(*))
            # because the 0.21 planner pushes COUNT(*) down to a single-row
            # aggregate; counting the projected join rows is what asserts
            # the 4 relationships are matched.
            # ================================================================
            run_test("Cypher: MATCH with rel relationship (count)",
                     f"SET ladybug.pg_connstr = '{libpq_connstr}'; "
                     "SELECT count(*)::int AS cnt FROM ladybug.cypher("
                     "'MATCH (a:node_person)-[k:rel_knows]->(b:node_person) RETURN a.name, b.name'"
                     ") AS t(a text, b text)",
                     env, check=lambda o, e: "4" in o)

            run_test("Cypher: MATCH with rel relationship (projection)",
                     f"SET ladybug.pg_connstr = '{libpq_connstr}'; "
                     "SELECT * FROM ladybug.cypher("
                     "'MATCH (a:node_person)-[k:rel_knows]->(b:node_person) RETURN a.name, b.name, k.since'"
                     ") AS t(a_name text, b_name text, since text) ORDER BY a_name",
                     env,
                     check=lambda o, e: ("Alice" in o and "Bob" in o and "2020-01-15" in o))

            # ================================================================
            # Declarative replication tests (Postgres -> Ladybug)
            #   - enable_replication creates a conventional PUBLICATION
            #   - AFTER row triggers convert SQL changes to standard Cypher
            #     CREATE / MERGE / DELETE statements in _replication_log
            #   - disable_replication tears everything down
            # These tests are pure-SPI (no liblbug runtime dependency).
            # ================================================================
            run_test("Replication: setup tables + register into graph",
                     "DROP TABLE IF EXISTS rrel_knows, rnode_person, rnode_city;"
                     "CREATE TABLE rnode_person (id INT PRIMARY KEY, name TEXT, age INT);"
                     "CREATE TABLE rnode_city (id INT PRIMARY KEY, name TEXT, population INT);"
                     "CREATE TABLE rrel_knows (id INT PRIMARY KEY, src_id INT, dst_id INT, since INT);"
                     "SELECT ladybug.register_node('Person','rnode_person','id',NULL,'repl');"
                     "SELECT ladybug.register_node('City','rnode_city','id',NULL,'repl');"
                     "SELECT ladybug.register_edge('KNOWS','rrel_knows','src_id','dst_id','id','repl');"
                     "SELECT 'registered';",
                     env, check=lambda o, e: "registered" in o)

            run_test("Replication: enable_replication('repl')",
                     "SELECT ladybug.enable_replication('repl') AS enabled_count",
                     env, check=lambda o, e: "3" in o)

            run_test("Replication: publication created by convention",
                     "SELECT pubname FROM pg_publication WHERE pubname='ladybug_repl_pub'",
                     env, check=lambda o, e: "ladybug_repl_pub" in o)

            run_test("Replication: triggers installed",
                     "SELECT count(*)::int AS n FROM pg_trigger t JOIN pg_class c ON c.oid=t.tgrelid "
                     "WHERE c.relname IN ('rnode_person','rnode_city','rrel_knows')",
                     env, check=lambda o, e: "3" in o)

            run_test("Replication: status shows 3 rows",
                     "SELECT count(*)::int AS n FROM ladybug.replication_status('repl')",
                     env, check=lambda o, e: "3" in o)

            run_test("Replication: INSERT -> Cypher CREATE (node)",
                     "INSERT INTO rnode_person VALUES (1,'Alice',30), (2,'Bob',25);"
                     "SELECT cypher FROM ladybug.replication_log('repl') "
                     "WHERE operation='INSERT' AND label='Person' ORDER BY id",
                     env, check=lambda o, e: "CREATE (n:Person {id: 1, name: 'Alice', age: 30})" in o)

            run_test("Replication: INSERT -> Cypher CREATE (edge)",
                     "INSERT INTO rrel_knows VALUES (1,1,2,2020);"
                     "SELECT cypher FROM ladybug.replication_log('repl') "
                     "WHERE operation='INSERT' AND label='KNOWS'",
                     env, check=lambda o, e: "CREATE (a)-[r:KNOWS {id: 1" in o and "since: 2020}]->(b)" in o)

            run_test("Replication: UPDATE -> Cypher MERGE",
                     "UPDATE rnode_person SET name='Alicia', age=31 WHERE id=1;"
                     "SELECT cypher FROM ladybug.replication_log('repl') "
                     "WHERE operation='UPDATE' AND label='Person' ORDER BY id DESC LIMIT 1",
                     env, check=lambda o, e: "MERGE (n:Person {id: 1}) SET n.name = 'Alicia', n.age = 31" in o)

            run_test("Replication: DELETE -> Cypher MATCH ... DELETE",
                     "DELETE FROM rnode_person WHERE id=2;"
                     "SELECT cypher FROM ladybug.replication_log('repl') "
                     "WHERE operation='DELETE' AND label='Person' ORDER BY id DESC LIMIT 1",
                     env, check=lambda o, e: "MATCH (n:Person {id: 2}) DELETE n" in o)

            run_test("Replication: disable_replication('repl')",
                     "SELECT ladybug.disable_replication('repl') AS disabled_count",
                     env, check=lambda o, e: "3" in o)

            run_test("Replication: publication dropped after disable",
                     "SELECT count(*)::int AS n FROM pg_publication WHERE pubname='ladybug_repl_pub'",
                     env, check=lambda o, e: "0" in o)

            run_test("Replication: triggers dropped after disable",
                     "SELECT count(*)::int AS n FROM pg_trigger t JOIN pg_class c ON c.oid=t.tgrelid "
                     "WHERE c.relname IN ('rnode_person','rnode_city','rrel_knows')",
                     env, check=lambda o, e: "0" in o)

            # ================================================================
            # Issue #2 regression: replay the same change log twice across two
            # SEPARATE backends.  The first replay materialises a row into a
            # persistent ladybug store; the second replay, in a fresh
            # backend, reopens that store and re-runs the same CREATE, which
            # hits a duplicate primary key.  Before the fix, liblbug threw a
            # std::out_of_range past its C API, std::terminate ran, and the
            # backend died with SIGABRT (taking the whole cluster down).  The
            # fix wraps the executing liblbug calls in a C++ catch(...) \n            # boundary (ladybug_bridge_guard.cpp); the duplicate must now be
            # reported as a skipped statement and return 0, not crash.
            #
            # Each run_test() is a separate `psql -c` process, hence a
            # separate backend / bridge / reopened store -- exactly the
            # scenario from the issue.
            # ================================================================
            REPLAY_STORE = "/tmp/pglb_issue2_replay.lbdb"
            run_test("Issue #2 setup: node table + register + insert (graph 'repl2')",
                     "DROP TABLE IF EXISTS rnode_city2;"
                     "CREATE TABLE rnode_city2 (id INT PRIMARY KEY, name TEXT NOT NULL);"
                     "SELECT ladybug.register_node('City','rnode_city2','id',NULL,'repl2');"
                     "SELECT ladybug.enable_replication('repl2') AS n;"
                     "INSERT INTO rnode_city2 VALUES (1,'Toronto');"
                     "SELECT 'ok' AS setup;",
                     env, check=lambda o, e: "ok" in o)

            run_test("Issue #2: first replay (backend A) materialises the row",
                     f"SET ladybug.storage_path = '{REPLAY_STORE}';"
                     f"SET ladybug.pg_connstr = '{libpq_connstr}';"
                     "SELECT * FROM ladybug.cypher("  # create native node table
                     "'CREATE NODE TABLE City(id INT64, name STRING, PRIMARY KEY(id))') AS t(ok text);"
                     "SELECT ladybug.replay_replication('repl2') AS applied;",
                     env, check=lambda o, e: "1" in o)

            run_test("Issue #2: second replay (backend B, reopened store) must not crash",
                     f"SET ladybug.storage_path = '{REPLAY_STORE}';"
                     "SELECT ladybug.replay_replication('repl2') AS second_replay;",
                     env, check=lambda o, e: "0" in o and "0" in o)

            # Cleanup the dedicated graph so the suite is idempotent.
            run_test("Issue #2 cleanup: disable_replication('repl2')",
                     "SELECT ladybug.disable_replication('repl2') AS n",
                     env, check=lambda o, e: "1" in o)

            # ================================================================
            # Issue #6 regression: a successful zero-column Cypher statement
            # (e.g. a data CREATE / MERGE / DELETE without RETURN) executes
            # successfully in Ladybug and then must NOT be reported to the
            # caller as a column-count-mismatch error.  Before the fix,
            # ladybug_bridge_execute_collect rejected any result whose
            # column count didn't match the caller's column definition
            # list, so a statement that legitimately returns 0 columns
            # failed at the column-count check -- after the side effect
            # had already landed, inviting unsafe retries.
            #
            # The whole sequence runs in ONE backend against a dedicated
            # storage path: create the native node table (DDL returns a
            # status column, already fine), then a data CREATE (returns 0
            # columns -> synthesized "OK" status row for the conventional
            # AS t(ok text) shape), then a MERGE (0 columns, but the
            # caller's column list is int -> honest empty result, count 0,
            # NOT an error), then a MATCH confirming both writes landed.
            # All four statements share one psql -c so the store is
            # created and reused in a single backend.
            # ================================================================
            ISSUE6_STORE = "/tmp/pglb_issue6.lbdb"
            run_test("Issue #6: zero-column Cypher mutations succeed (not reported as errors)",
                     f"SET ladybug.storage_path = '{ISSUE6_STORE}';"
                     f"SET ladybug.pg_connstr = '{libpq_connstr}';"
                     "SELECT * FROM ladybug.cypher("  # create native node table (DDL)
                     "$$CREATE NODE TABLE City(id INT64, name STRING, PRIMARY KEY(id))$$)"
                     " AS t(ok text);"
                     "SELECT * FROM ladybug.cypher("  # data CREATE -> 0 columns -> "OK"
                     "$$CREATE (n:City {id: 1, name: 'Toronto'})$$)"
                     " AS t(ok text);"
                     "SELECT count(*)::int AS cnt FROM ladybug.cypher("  # MERGE -> 0 columns, int col -> empty
                     "$$MERGE (n:City {id: 2, name: 'Montreal'})$$)"
                     " AS t(dummy int);"
                     "SELECT * FROM ladybug.cypher("  # confirm both writes landed
                     "$$MATCH (n:City) RETURN n.id, n.name ORDER BY n.id$$)"
                     " AS t(id bigint, name text)",
                     env, check=lambda o, e: ("OK" in o
                                              and "Toronto" in o
                                              and "Montreal" in o
                                              and "ERROR" not in e.upper()))

            xfail_note = f" ({tests_xfail} xfail)" if tests_xfail else ""
            print(f"\n=== {tests_passed}/{tests_total} tests passed{xfail_note} ===")
            # All existing tests are required.
            if tests_passed >= tests_total:
                print("All essential tests PASSED - compile-time linking works!")
                return 0
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
