#!/usr/bin/env python3
"""Scratch PostgreSQL cluster provisioning for the E2E tests.

Uses a system PostgreSQL installation (initdb / pg_ctl / createdb / psql on
PATH). Each run creates an isolated cluster under a temp directory on a
high port, so no root and no interference with any system cluster is needed.
"""

import os
import shutil
import socket
import subprocess
import tempfile

TOOLS = ("initdb", "pg_ctl", "createdb", "psql")


def check_tools():
    """Exit with a clear message if the system postgres tools are missing."""
    missing = [t for t in TOOLS if shutil.which(t) is None]
    if missing:
        raise SystemExit(
            "E2E tests need a system PostgreSQL server, but these are missing "
            "from PATH: %s\n"
            "Install them, e.g. on Fedora:  sudo dnf install postgresql-server"
            % ", ".join(missing))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class PgCluster:
    def __init__(self, tmpdir=None, port=None, dbname="gjtest"):
        self.tmpdir = tmpdir or tempfile.mkdtemp(prefix="gj-e2e-")
        self.datadir = os.path.join(self.tmpdir, "pgdata")
        self.sockdir = os.path.join(self.tmpdir, "pgsock")
        self.port = port or free_port()
        self.dbname = dbname
        self.host = "127.0.0.1"
        self.user = "postgres"
        self.logfile = os.path.join(self.tmpdir, "pg.log")

    @property
    def conn(self):
        return "host=%s port=%d user=%s dbname=%s" % (
            self.host, self.port, self.user, self.dbname)

    def start(self):
        os.makedirs(self.sockdir, exist_ok=True)
        subprocess.run(["initdb", "-D", self.datadir, "-U", self.user,
                        "-A", "trust", "--no-sync"],
                       check=True, capture_output=True)
        subprocess.run(
            ["pg_ctl", "-D", self.datadir,
             "-o", "-p %d -k %s -c listen_addresses=%s"
             % (self.port, self.sockdir, self.host),
             "-l", self.logfile, "-w", "start"],
            check=True, capture_output=True)
        subprocess.run(["createdb", "-h", self.host, "-p", str(self.port),
                        "-U", self.user, self.dbname],
                       check=True, capture_output=True)
        return self

    def psql(self, sql):
        """Run SQL and return rows as a list of tuples of strings."""
        out = subprocess.run(
            ["psql", "-h", self.host, "-p", str(self.port), "-U", self.user,
             "-d", self.dbname, "-At", "-F", "\t", "-c", sql],
            check=True, capture_output=True, text=True)
        return [tuple(line.split("\t")) for line in out.stdout.splitlines()
                if line != ""]

    def stop(self):
        subprocess.run(["pg_ctl", "-D", self.datadir, "-m", "fast", "stop"],
                       capture_output=True)

    def cleanup(self):
        self.stop()
        shutil.rmtree(self.tmpdir, ignore_errors=True)