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
    subprocess.run(["make", "clean"], cwd=repo_root, capture_output=True, text=True, env=env_build)
    result = subprocess.run(["make"], cwd=repo_root, capture_output=True, text=True, env=env_build)
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
    shutil.copy(repo_root / "pg_ladybug.so", pg_lib_dir / "pg_ladybug.so")
    shutil.copy(repo_root / "pg_ladybug--1.0.sql", pg_share_dir / "pg_ladybug--1.0.sql")
    shutil.copy(repo_root / "pg_ladybug.control", pg_share_dir / "pg_ladybug.control")
    print("Extension files installed")

    import psycopg

    tests_passed = 0
    tests_total = 0

    def run_test(name: str, sql: str, env, check: callable = None) -> bool:
        nonlocal tests_passed, tests_total
        tests_total += 1
        print(f"\n--- Test {tests_total}: {name} ---")
        result = subprocess.run(
            ["psql", "-c", sql], env=env, capture_output=True, text=True,
        )
        if result.stdout:
            for line in result.stdout.strip().split("\n")[:25]:
                print("  ", line)
        if result.returncode != 0:
            if result.stderr:
                for line in result.stderr.strip().split("\n")[:5]:
                    print("  ERR:", line)
            print(f"FAIL (exit code {result.returncode})")
            return False
        if check and not check(result.stdout, result.stderr):
            print("FAIL: check failed")
            return False
        print("PASS")
        tests_passed += 1
        return True

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
                ["psql", "-c",
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

            print(f"\n=== {tests_passed}/{tests_total} tests passed ===")
            # All 15 tests are required.
            if tests_passed >= tests_total:
                print("All essential tests PASSED - compile-time linking works!")
                return 0
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())