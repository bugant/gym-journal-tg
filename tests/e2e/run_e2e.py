#!/usr/bin/env python3
"""End-to-end test driver for the gym journal Telegram bot.

Runs the real bot binary against a local mock Telegram API server and a
scratch PostgreSQL cluster, then plays the scenarios in scenarios.py back
to back, asserting on the bot's replies and on the database contents.

Usage:
    python3 tests/e2e/run_e2e.py [--bot ./gymjournal] [--filter new]
                                 [--verbose] [--keep]

The `make e2e` target wraps this.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from http.server import ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))

from mock_tg import Handler, MockTelegram  # noqa: E402
from pg_setup import PgCluster, check_tools  # noqa: E402
from scenarios import SCENARIOS  # noqa: E402

TOKEN = "123456:TESTTOKEN"
REPLY_TIMEOUT = 15.0
READY_TIMEOUT = 15.0


class Reporter:
    def __init__(self, verbose=False):
        self.verbose = verbose
        self.passed = 0
        self.failed = 0
        self.failures = []

    def step_ok(self, msg):
        if self.verbose:
            print("      ok: %s" % msg, flush=True)

    def scenario_ok(self, name):
        self.passed += 1
        print("PASS  %s" % name, flush=True)

    def scenario_fail(self, name, detail):
        self.failed += 1
        self.failures.append((name, detail))
        print("FAIL  %s" % name, flush=True)
        print("      %s" % detail.replace("\n", "\n      "), flush=True)


def wait_for(pred, timeout, what):
    deadline = time.time() + timeout
    while time.time() < deadline:
        val = pred()
        if val:
            return val
        time.sleep(0.03)
    raise TimeoutError("timed out waiting for %s" % what)


def check_reply(actual, step):
    if step.expect is not None:
        return actual == step.expect
    if step.expect_re is not None:
        return re.search(step.expect_re, actual) is not None
    if step.contains:
        return all(c in actual for c in step.contains)
    return True


def describe_expect(step):
    if step.expect is not None:
        return repr(step.expect)
    if step.expect_re is not None:
        return "regex %r" % step.expect_re
    if step.contains:
        return "contains %r" % step.contains
    return "<no check>"


def run_scenario(scenario, mock, pg, reporter):
    mock.reset()
    pg.psql("TRUNCATE users, sessions, sets, KeyValue RESTART IDENTITY")

    chat_counts = {}
    for step in scenario.steps:
        chat_counts[step.chat] = chat_counts.get(step.chat, 0) + 1
        expected = chat_counts[step.chat]

        mock.enqueue({"text": step.send, "chat_id": step.chat,
                      "from_id": step.sender, "username": step.username})

        def sent_for_chat():
            return [m for m in mock.sent_log()
                    if m["method"] == "sendMessage"
                    and str(m["chat_id"]) == str(step.chat)]

        try:
            msgs = wait_for(lambda: len(sent_for_chat()) >= expected,
                            REPLY_TIMEOUT,
                            "reply to %r" % step.send)
        except TimeoutError as e:
            return False, "%s (saw %d of %d replies)" % (
                e, len(sent_for_chat()), expected)

        actual = sent_for_chat()[expected - 1]["text"]
        if not check_reply(actual, step):
            return False, ("reply mismatch for %r\n"
                           "  expected: %s\n"
                           "  actual:   %r") % (
                step.send, describe_expect(step), actual)

        # Replies must be sent as plain text: parse_mode=Markdown would make
        # Telegram reject replies containing unbalanced special characters
        # ("kg*rep", "[vol 1920]", ...) and the user would see no reply.
        pm = sent_for_chat()[expected - 1].get("parse_mode", "")
        if pm == "Markdown":
            return False, ("reply for %r was sent with parse_mode=Markdown; "
                           "Telegram may reject it (use plain text)" % step.send)

        for sql, expected_rows in step.db:
            rows = pg.psql(sql)
            if rows != expected_rows:
                return False, ("db mismatch after %r\n"
                               "  sql:      %s\n"
                               "  expected: %r\n"
                               "  actual:   %r") % (
                    step.send, sql, expected_rows, rows)

        reporter.step_ok("%r -> %r" % (step.send, actual))

    time.sleep(0.05)  # let the handler thread release the db lock
    return True, ""


def dump_state(mock, pg):
    print("  --- mock sent log ---", flush=True)
    for m in mock.sent_log():
        print("    %s" % m, flush=True)
    print("  --- database ---", flush=True)
    for table in ("users", "sessions", "sets", "KeyValue"):
        try:
            rows = pg.psql("SELECT * FROM %s ORDER BY 1" % table)
        except subprocess.CalledProcessError:
            rows = []
        print("    %s: %r" % (table, rows), flush=True)


def spawn_bot(bot, mock_port, pg, logfile):
    env = dict(os.environ)
    env["TELEGRAM_API_URL"] = "http://127.0.0.1:%d" % mock_port
    cmd = [str(bot), "--apikey", TOKEN, "--dburl", pg.conn, "--debug"]
    # Force line buffering so the log is usable even if we SIGKILL the bot.
    stdbuf = subprocess.run(["sh", "-c", "command -v stdbuf || true"],
                            capture_output=True, text=True).stdout.strip()
    if stdbuf:
        cmd = ["stdbuf", "-oL", "-eL"] + cmd
    logfh = open(logfile, "ab", buffering=0)
    proc = subprocess.Popen(cmd, env=env, cwd=str(REPO),
                            stdout=logfh, stderr=subprocess.STDOUT)
    # Keep the file object alive for the child's lifetime.
    proc._logfile = logfh
    return proc


def bot_log_tail(logfile, n=40):
    try:
        with open(logfile, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - 8192))
            return f.read().decode("utf-8", "replace")
    except OSError:
        return "<no bot log>"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bot", default=str(REPO / "gymjournal"),
                    help="path to the bot binary (default: %(default)s)")
    ap.add_argument("--filter", default="",
                    help="only run scenarios whose name contains this string")
    ap.add_argument("--list-scenarios", action="store_true",
                    help="print scenario names and exit")
    ap.add_argument("--verbose", action="store_true",
                    help="verbose per-step output")
    ap.add_argument("--keep", action="store_true",
                    help="keep pg + tmpdir + bot running on failure for inspection")
    args = ap.parse_args(argv)

    if args.list_scenarios:
        for s in SCENARIOS:
            print(s.name)
        return 0

    if not os.path.exists(args.bot):
        print("bot binary not found: %s (run `make` first)" % args.bot)
        return 2

    check_tools()

    workdir = tempfile.mkdtemp(prefix="gje2e-")
    bot_log = os.path.join(workdir, "bot.log")
    pg = PgCluster(tmpdir=os.path.join(workdir, "pg"))
    proc = None
    mock = None
    server = None

    try:
        pg.start()
        print("postgres: %s" % pg.conn, flush=True)

        mock = MockTelegram(token=TOKEN)
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.state = mock
        server.debug = args.verbose
        port = server.server_address[1]
        threading.Thread(target=server.serve_forever, daemon=True).start()
        print("mock telegram: http://127.0.0.1:%d" % port, flush=True)

        proc = spawn_bot(args.bot, port, pg, bot_log)
        try:
            wait_for(lambda: mock.ready, READY_TIMEOUT, "bot to come up")
        except TimeoutError:
            print("bot did not come up. Log tail:\n%s"
                  % bot_log_tail(bot_log), flush=True)
            return 1
        print("bot: up (pid %d)" % proc.pid, flush=True)

        scenarios = [s for s in SCENARIOS if args.filter in s.name]
        if not scenarios:
            print("no scenarios match filter %r" % args.filter)
            return 2

        reporter = Reporter(verbose=args.verbose)
        for scenario in scenarios:
            if proc.poll() is not None:
                print("bot exited unexpectedly. Log tail:\n%s"
                      % bot_log_tail(bot_log), flush=True)
                return 1
            ok, detail = run_scenario(scenario, mock, pg, reporter)
            if ok:
                reporter.scenario_ok(scenario.name)
            else:
                reporter.scenario_fail(scenario.name, detail)
                dump_state(mock, pg)
                if not args.keep:
                    return 1

        print("\nran %d scenarios, %d passed, %d failed"
              % (reporter.passed + reporter.failed,
                 reporter.passed, reporter.failed), flush=True)
        return 0 if reporter.failed == 0 else 1
    except Exception as e:  # noqa: BLE001
        print("driver error: %r" % e, flush=True)
        if proc is not None:
            print("bot log tail:\n%s" % bot_log_tail(bot_log), flush=True)
        return 1
    finally:
        if not args.keep and proc is not None and proc.poll() is None:
            proc.kill()
            proc.wait()
        if server is not None:
            server.shutdown()
        if args.keep:
            print("kept for inspection:", flush=True)
            print("  bot log:   %s" % bot_log, flush=True)
            print("  pg dir:    %s" % pg.datadir, flush=True)
            print("  dburl:     %s" % pg.conn, flush=True)
        else:
            pg.cleanup()
            import shutil
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())