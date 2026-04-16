#!/usr/bin/env python3
"""
Anunix superrouter — hardware profile library and driver stub registry.

HTTP/JSON API over SQLite. Standard library only.
Default port: 8420. See RFC-0011 for full API specification.

Usage:
    python3 server.py [--port PORT] [--db PATH] [--admin-secret SECRET]
"""

import hashlib
import http.server
import json
import logging
import os
import sqlite3
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlparse, parse_qs

VERSION = "0.1.0"
DEFAULT_PORT = 8420
DEFAULT_DB = "/var/lib/anunix-superrouter/db.sqlite3"
SCHEMA_FILE = Path(__file__).parent / "schema.sql"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("superrouter")


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
    with open(SCHEMA_FILE) as f:
        conn.executescript(f.read())
    conn.commit()
    return conn


class Handler(http.server.BaseHTTPRequestHandler):

    db: sqlite3.Connection
    admin_secret: str

    def log_message(self, fmt, *args):
        log.info("%s %s", self.address_string(), fmt % args)

    def send_json(self, code: int, body: object):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_error_json(self, code: int, message: str):
        self.send_json(code, {"error": message})

    def read_body(self) -> dict | None:
        length = int(self.headers.get("Content-Length", 0))
        if not length:
            return {}
        try:
            return json.loads(self.rfile.read(length))
        except json.JSONDecodeError:
            return None

    def auth_node(self) -> str | None:
        """Return node_id for a valid bearer token, or None."""
        header = self.headers.get("Authorization", "")
        if not header.startswith("Bearer "):
            return None
        raw = header[7:]
        h = hash_token(raw)
        row = self.db.execute(
            "SELECT node_id FROM api_tokens WHERE token_hash=? AND revoked=0",
            (h,)
        ).fetchone()
        if row:
            self.db.execute(
                "UPDATE api_tokens SET last_used=? WHERE token_hash=?",
                (now_iso(), h)
            )
            self.db.commit()
            return row["node_id"]
        return None

    def auth_admin(self) -> bool:
        header = self.headers.get("Authorization", "")
        if not header.startswith("Bearer "):
            return False
        return header[7:] == self.admin_secret

    # ------------------------------------------------------------------ #
    # Routing                                                              #
    # ------------------------------------------------------------------ #

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/")
        qs = parse_qs(parsed.query)

        if path == "/v1/health":
            self.send_json(200, {"status": "ok", "version": VERSION})
            return

        node_id = self.auth_node()
        if not node_id:
            self.send_error_json(401, "unauthorized")
            return

        if path.startswith("/v1/profiles"):
            self._get_profiles(path, qs)
        elif path.startswith("/v1/stubs"):
            self._get_stubs(path, qs)
        else:
            self.send_error_json(404, "not found")

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/")

        if path == "/v1/admin/tokens":
            if not self.auth_admin():
                self.send_error_json(401, "unauthorized")
                return
            self._provision_token()
            return

        node_id = self.auth_node()
        if not node_id:
            self.send_error_json(401, "unauthorized")
            return

        if path == "/v1/profiles":
            self._post_profile(node_id)
        elif path == "/v1/stubs":
            self._post_stub(node_id)
        elif path.startswith("/v1/stubs/") and path.endswith("/download"):
            stub_id = path.split("/")[3]
            self._record_download(stub_id)
        else:
            self.send_error_json(404, "not found")

    # ------------------------------------------------------------------ #
    # Profile handlers                                                     #
    # ------------------------------------------------------------------ #

    def _get_profiles(self, path: str, qs: dict):
        parts = path.split("/")
        if len(parts) == 4 and parts[3]:
            row = self.db.execute(
                "SELECT payload FROM profiles WHERE profile_id=?",
                (parts[3],)
            ).fetchone()
            if row:
                self.send_json(200, json.loads(row["payload"]))
            else:
                self.send_error_json(404, "profile not found")
            return

        clauses, params = [], []
        if "node_id" in qs:
            clauses.append("node_id=?"); params.append(qs["node_id"][0])
        if "arch" in qs:
            clauses.append("arch=?"); params.append(qs["arch"][0])

        where = ("WHERE " + " AND ".join(clauses)) if clauses else ""
        rows = self.db.execute(
            f"SELECT payload FROM profiles {where} ORDER BY submitted_at DESC",
            params
        ).fetchall()
        self.send_json(200, [json.loads(r["payload"]) for r in rows])

    def _post_profile(self, node_id: str):
        body = self.read_body()
        if body is None:
            self.send_error_json(400, "invalid JSON")
            return

        body["node_id"] = node_id
        body.setdefault("submitted_at", now_iso())
        body.setdefault("arch", "unknown")

        existing = self.db.execute(
            "SELECT profile_id, profile_version FROM profiles WHERE node_id=? ORDER BY submitted_at DESC LIMIT 1",
            (node_id,)
        ).fetchone()

        version = (existing["profile_version"] + 1) if existing else 1
        profile_id = str(uuid.uuid4())
        body["profile_id"] = profile_id
        body["profile_version"] = version

        self.db.execute(
            "INSERT INTO profiles(profile_id,node_id,hostname,arch,submitted_at,profile_version,payload) VALUES(?,?,?,?,?,?,?)",
            (profile_id, node_id, body.get("hostname"), body["arch"],
             body["submitted_at"], version, json.dumps(body))
        )
        self.db.commit()

        new_stubs_requested = self._check_missing_stubs(body)
        self.send_json(201, {
            "profile_id": profile_id,
            "profile_version": version,
            "new_stubs_requested": new_stubs_requested,
        })

    def _check_missing_stubs(self, profile: dict) -> bool:
        for dev in profile.get("pci_devices", []):
            row = self.db.execute(
                "SELECT stub_id FROM driver_stubs WHERE vendor_id=? AND device_id=? AND arch=? AND superseded_by IS NULL LIMIT 1",
                (dev.get("vendor_id"), dev.get("device_id"), profile.get("arch"))
            ).fetchone()
            if not row:
                return True
        return False

    # ------------------------------------------------------------------ #
    # Stub handlers                                                        #
    # ------------------------------------------------------------------ #

    def _get_stubs(self, path: str, qs: dict):
        parts = path.split("/")
        if len(parts) == 4 and parts[3] and not parts[3].startswith("?"):
            row = self.db.execute(
                "SELECT * FROM driver_stubs WHERE stub_id=?",
                (parts[3],)
            ).fetchone()
            if row:
                self.send_json(200, dict(row))
            else:
                self.send_error_json(404, "stub not found")
            return

        clauses, params = ["superseded_by IS NULL"], []
        if "vendor_id" in qs:
            clauses.append("vendor_id=?"); params.append(qs["vendor_id"][0])
        if "device_id" in qs:
            clauses.append("device_id=?"); params.append(qs["device_id"][0])
        if "arch" in qs:
            clauses.append("arch=?"); params.append(qs["arch"][0])

        where = "WHERE " + " AND ".join(clauses)
        rows = self.db.execute(
            f"SELECT * FROM driver_stubs {where} ORDER BY generated_at DESC",
            params
        ).fetchall()
        self.send_json(200, [dict(r) for r in rows])

    def _post_stub(self, _node_id: str):
        body = self.read_body()
        if body is None:
            self.send_error_json(400, "invalid JSON")
            return

        required = ("vendor_id", "device_id", "class_code", "arch",
                    "profile_id", "stub_text", "generated_by")
        for field in required:
            if field not in body:
                self.send_error_json(400, f"missing field: {field}")
                return

        stub_id = str(uuid.uuid4())
        generated_at = body.get("generated_at", now_iso())

        self.db.execute(
            "INSERT INTO driver_stubs(stub_id,vendor_id,device_id,class_code,arch,profile_id,stub_text,generated_at,generated_by,stub_oid) VALUES(?,?,?,?,?,?,?,?,?,?)",
            (stub_id, body["vendor_id"], body["device_id"], body["class_code"],
             body["arch"], body["profile_id"], body["stub_text"],
             generated_at, body["generated_by"], body.get("stub_oid"))
        )
        self.db.commit()

        stub_dir = Path(DEFAULT_DB).parent / "stubs"
        stub_dir.mkdir(parents=True, exist_ok=True)
        (stub_dir / f"{stub_id}.c").write_text(body["stub_text"])

        self.send_json(201, {"stub_id": stub_id})

    def _record_download(self, stub_id: str):
        result = self.db.execute(
            "UPDATE driver_stubs SET download_count=download_count+1 WHERE stub_id=?",
            (stub_id,)
        )
        self.db.commit()
        if result.rowcount == 0:
            self.send_error_json(404, "stub not found")
            return
        self.send_response(204)
        self.end_headers()

    # ------------------------------------------------------------------ #
    # Admin                                                                #
    # ------------------------------------------------------------------ #

    def _provision_token(self):
        body = self.read_body()
        if body is None:
            self.send_error_json(400, "invalid JSON")
            return
        node_id = body.get("node_id")
        if not node_id:
            self.send_error_json(400, "missing node_id")
            return

        raw = str(uuid.uuid4())
        self.db.execute(
            "INSERT INTO api_tokens(token_hash,node_id,hostname,created_at) VALUES(?,?,?,?)",
            (hash_token(raw), node_id, body.get("hostname"), now_iso())
        )
        self.db.commit()
        self.send_json(201, {"token": raw})


def make_handler(db: sqlite3.Connection, admin_secret: str):
    class BoundHandler(Handler):
        pass
    BoundHandler.db = db
    BoundHandler.admin_secret = admin_secret
    return BoundHandler


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Anunix superrouter server")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--db", default=os.environ.get("SUPERROUTER_DB", DEFAULT_DB))
    parser.add_argument("--admin-secret", default=os.environ.get("SUPERROUTER_ADMIN_SECRET", ""))
    args = parser.parse_args()

    if not args.admin_secret:
        log.warning("No admin secret configured — token provisioning disabled")

    db = open_db(args.db)
    handler = make_handler(db, args.admin_secret)
    server = http.server.HTTPServer(("0.0.0.0", args.port), handler)
    log.info("superrouter v%s listening on port %d", VERSION, args.port)
    log.info("database: %s", args.db)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("shutting down")
        db.close()


if __name__ == "__main__":
    main()
