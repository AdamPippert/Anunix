#!/usr/bin/env python3
"""
Anunix superrouter — workflow distribution, hardware profiles, and OS updates.

HTTP/JSON API over SQLite. Standard library only.
Default port: 8420.

Usage:
    python3 server.py [--port PORT] [--db PATH] [--blobs DIR]
"""

import hashlib
import http.server
import json
import logging
import os
import sqlite3
import struct
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlparse, parse_qs

VERSION = "0.2.0"
DEFAULT_PORT = 8420
DEFAULT_DB = "/var/lib/anunix-superrouter/db.sqlite3"
DEFAULT_BLOBS = "/var/lib/anunix-superrouter/blobs"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("superrouter")

SCHEMA = """
CREATE TABLE IF NOT EXISTS workflows (
    uri          TEXT PRIMARY KEY,
    display_name TEXT,
    description  TEXT,
    blob_path    TEXT,
    blob_size    INTEGER,
    created_at   TEXT
);

CREATE TABLE IF NOT EXISTS tokens (
    token_hash TEXT PRIMARY KEY,
    label      TEXT,
    created_at TEXT
);

CREATE TABLE IF NOT EXISTS hw_profiles (
    profile_id   TEXT PRIMARY KEY,
    node_id      TEXT,
    hostname     TEXT,
    arch         TEXT,
    submitted_at TEXT,
    profile_json TEXT
);

CREATE TABLE IF NOT EXISTS updates (
    channel   TEXT,
    arch      TEXT,
    version   TEXT,
    blob_path TEXT,
    blob_size INTEGER,
    PRIMARY KEY (channel, arch)
);
"""


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def hash_token(raw: str) -> str:
    return hashlib.sha256(raw.encode()).hexdigest()


def open_db(path: str) -> sqlite3.Connection:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(path, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    conn.executescript(SCHEMA)
    conn.commit()
    return conn


def _check_token(db: sqlite3.Connection, headers) -> bool:
    header = headers.get("Authorization", "")
    if not header.startswith("Bearer "):
        return False
    h = hash_token(header[7:])
    row = db.execute(
        "SELECT token_hash FROM tokens WHERE token_hash=?", (h,)
    ).fetchone()
    return row is not None


class Handler(http.server.BaseHTTPRequestHandler):

    db: sqlite3.Connection
    blobs_dir: str

    def log_message(self, fmt, *args):
        log.info("%s %s", self.address_string(), fmt % args)

    def send_json(self, code: int, body: object):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_blob(self, code: int, data: bytes, content_type: str = "application/octet-stream"):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_err(self, code: int, message: str):
        self.send_json(code, {"error": message})

    def read_raw(self) -> bytes:
        length = int(self.headers.get("Content-Length", 0))
        if not length:
            return b""
        return self.rfile.read(length)

    def read_json(self):
        data = self.read_raw()
        if not data:
            return {}
        try:
            return json.loads(data)
        except json.JSONDecodeError:
            return None

    def authed(self) -> bool:
        return _check_token(self.db, self.headers)

    # ------------------------------------------------------------------ #
    # GET routing                                                          #
    # ------------------------------------------------------------------ #

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        # Public: workflow listing
        if path == "/workflows/" or path == "/workflows":
            self._get_workflow_list()
            return

        # Public: fetch individual workflow blob
        if path.startswith("/workflows/"):
            uri = path[len("/workflows/"):]
            if uri:
                self._get_workflow(uri)
                return

        # Public: OS updates
        if path.startswith("/updates/"):
            self._get_update(path)
            return

        self.send_err(404, "not found")

    def _get_workflow_list(self):
        rows = self.db.execute("SELECT uri FROM workflows ORDER BY uri").fetchall()
        self.send_json(200, [r["uri"] for r in rows])

    def _get_workflow(self, uri: str):
        row = self.db.execute(
            "SELECT blob_path FROM workflows WHERE uri=?", (uri,)
        ).fetchone()
        if not row:
            self.send_err(404, "workflow not found")
            return
        try:
            data = Path(row["blob_path"]).read_bytes()
        except OSError:
            self.send_err(500, "blob missing")
            return
        self.send_blob(200, data)

    def _get_update(self, path: str):
        # /updates/<channel>/<arch>/anunix.bin  or  /updates/<channel>/<arch>/manifest.json
        parts = [p for p in path.split("/") if p]
        if len(parts) != 4 or parts[0] != "updates":
            self.send_err(404, "not found")
            return
        _, channel, arch, filename = parts
        row = self.db.execute(
            "SELECT version, blob_path, blob_size FROM updates WHERE channel=? AND arch=?",
            (channel, arch)
        ).fetchone()
        if not row:
            self.send_err(404, "no update for channel/arch")
            return
        if filename == "manifest.json":
            self.send_json(200, {"version": row["version"], "size": row["blob_size"]})
        elif filename == "anunix.bin":
            try:
                data = Path(row["blob_path"]).read_bytes()
            except OSError:
                self.send_err(500, "blob missing")
                return
            self.send_blob(200, data)
        else:
            self.send_err(404, "not found")

    # ------------------------------------------------------------------ #
    # PUT routing                                                          #
    # ------------------------------------------------------------------ #

    def do_PUT(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if not self.authed():
            self.send_err(401, "unauthorized")
            return

        if path.startswith("/workflows/"):
            uri = path[len("/workflows/"):]
            if uri:
                self._put_workflow(uri)
                return

        if path.startswith("/updates/"):
            self._put_update(path)
            return

        self.send_err(404, "not found")

    def _put_workflow(self, uri: str):
        data = self.read_raw()
        if not data:
            self.send_err(400, "empty body")
            return
        blob_dir = Path(self.blobs_dir) / "workflows"
        blob_dir.mkdir(parents=True, exist_ok=True)
        safe = uri.replace("/", "_").replace(":", "_")
        blob_path = str(blob_dir / f"{safe}.wf")
        Path(blob_path).write_bytes(data)
        self.db.execute(
            "INSERT OR REPLACE INTO workflows(uri, blob_path, blob_size, created_at) VALUES(?,?,?,?)",
            (uri, blob_path, len(data), now_iso())
        )
        self.db.commit()
        self.send_json(201, {"uri": uri, "size": len(data)})

    def _put_update(self, path: str):
        parts = [p for p in path.split("/") if p]
        # /updates/<channel>/<arch>/anunix.bin
        if len(parts) != 4 or parts[0] != "updates" or parts[3] != "anunix.bin":
            self.send_err(404, "not found")
            return
        _, channel, arch, _ = parts
        version = self.headers.get("X-Anunix-Version", "unknown")
        data = self.read_raw()
        if not data:
            self.send_err(400, "empty body")
            return
        blob_dir = Path(self.blobs_dir) / "updates" / channel / arch
        blob_dir.mkdir(parents=True, exist_ok=True)
        blob_path = str(blob_dir / "anunix.bin")
        Path(blob_path).write_bytes(data)
        self.db.execute(
            "INSERT OR REPLACE INTO updates(channel, arch, version, blob_path, blob_size) VALUES(?,?,?,?,?)",
            (channel, arch, version, blob_path, len(data))
        )
        self.db.commit()
        self.send_json(201, {"channel": channel, "arch": arch, "version": version, "size": len(data)})

    # ------------------------------------------------------------------ #
    # POST routing                                                         #
    # ------------------------------------------------------------------ #

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/hw-profiles/" or path == "/hw-profiles":
            self._post_hw_profile()
            return

        if path == "/tokens/" or path == "/tokens":
            self._post_token()
            return

        self.send_err(404, "not found")

    def _post_hw_profile(self):
        body = self.read_json()
        if body is None:
            self.send_err(400, "invalid JSON")
            return
        profile_id = str(uuid.uuid4())
        submitted_at = now_iso()
        self.db.execute(
            "INSERT INTO hw_profiles(profile_id, node_id, hostname, arch, submitted_at, profile_json) VALUES(?,?,?,?,?,?)",
            (profile_id,
             body.get("node_id"),
             body.get("hostname"),
             body.get("arch"),
             submitted_at,
             json.dumps(body))
        )
        self.db.commit()
        self.send_json(201, {"profile_id": profile_id})

    def _post_token(self):
        # Bootstrap: allow creation when tokens table is empty; otherwise require auth.
        count = self.db.execute("SELECT COUNT(*) FROM tokens").fetchone()[0]
        if count > 0 and not self.authed():
            self.send_err(401, "unauthorized")
            return
        body = self.read_json()
        label = body.get("label", "") if body else ""
        raw = str(uuid.uuid4())
        self.db.execute(
            "INSERT INTO tokens(token_hash, label, created_at) VALUES(?,?,?)",
            (hash_token(raw), label, now_iso())
        )
        self.db.commit()
        self.send_json(201, {"token": raw, "label": label})


def make_handler(db: sqlite3.Connection, blobs_dir: str):
    class BoundHandler(Handler):
        pass
    BoundHandler.db = db
    BoundHandler.blobs_dir = blobs_dir
    return BoundHandler


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Anunix superrouter")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--db", default=os.environ.get("SUPERROUTER_DB", DEFAULT_DB))
    parser.add_argument("--blobs", default=os.environ.get("SUPERROUTER_BLOBS", DEFAULT_BLOBS))
    args = parser.parse_args()

    Path(args.blobs).mkdir(parents=True, exist_ok=True)
    db = open_db(args.db)
    handler = make_handler(db, args.blobs)
    server = http.server.HTTPServer(("0.0.0.0", args.port), handler)
    log.info("superrouter v%s listening on :%d", VERSION, args.port)
    log.info("db=%s  blobs=%s", args.db, args.blobs)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("shutting down")
        db.close()


if __name__ == "__main__":
    main()
