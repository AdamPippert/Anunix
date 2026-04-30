#!/usr/bin/env python3
"""
anunix-distd management CLI.

Commands:
    add-token <label>                          — generate token, store hash, print token
    add-workflow <uri> <blob_path>             — store a workflow blob
    add-update <channel> <arch> <version> <bin_path> — store an OS update image
    list-workflows                             — print all workflow URIs
"""

import hashlib
import json
import os
import shutil
import sqlite3
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path

DEFAULT_DB = "/var/lib/anunix-distd/db.sqlite3"
DEFAULT_BLOBS = "/var/lib/anunix-distd/blobs"

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
    conn = sqlite3.connect(path)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.executescript(SCHEMA)
    conn.commit()
    return conn


def cmd_add_token(db: sqlite3.Connection, label: str):
    raw = str(uuid.uuid4())
    db.execute(
        "INSERT INTO tokens(token_hash, label, created_at) VALUES(?,?,?)",
        (hash_token(raw), label, now_iso())
    )
    db.commit()
    print(raw)


def cmd_add_workflow(db: sqlite3.Connection, blobs_dir: str, uri: str, blob_path: str):
    src = Path(blob_path)
    if not src.exists():
        print(f"error: {blob_path} not found", file=sys.stderr)
        sys.exit(1)
    dest_dir = Path(blobs_dir) / "workflows"
    dest_dir.mkdir(parents=True, exist_ok=True)
    safe = uri.replace("/", "_").replace(":", "_")
    dest = dest_dir / f"{safe}.wf"
    shutil.copy2(src, dest)
    db.execute(
        "INSERT OR REPLACE INTO workflows(uri, blob_path, blob_size, created_at) VALUES(?,?,?,?)",
        (uri, str(dest), src.stat().st_size, now_iso())
    )
    db.commit()
    print(f"stored workflow {uri} ({src.stat().st_size} bytes)")


def cmd_add_update(db: sqlite3.Connection, blobs_dir: str,
                   channel: str, arch: str, version: str, bin_path: str):
    src = Path(bin_path)
    if not src.exists():
        print(f"error: {bin_path} not found", file=sys.stderr)
        sys.exit(1)
    dest_dir = Path(blobs_dir) / "updates" / channel / arch
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / "anunix.bin"
    shutil.copy2(src, dest)
    db.execute(
        "INSERT OR REPLACE INTO updates(channel, arch, version, blob_path, blob_size) VALUES(?,?,?,?,?)",
        (channel, arch, version, str(dest), src.stat().st_size)
    )
    db.commit()
    print(f"stored update {channel}/{arch} version={version} ({src.stat().st_size} bytes)")


def cmd_list_workflows(db: sqlite3.Connection):
    rows = db.execute("SELECT uri FROM workflows ORDER BY uri").fetchall()
    if not rows:
        print("(no workflows)")
        return
    for row in rows:
        print(row["uri"])


def usage():
    print(__doc__.strip())
    sys.exit(1)


def main():
    db_path = os.environ.get("SUPERROUTER_DB", DEFAULT_DB)
    blobs_dir = os.environ.get("SUPERROUTER_BLOBS", DEFAULT_BLOBS)
    db = open_db(db_path)

    args = sys.argv[1:]
    if not args:
        usage()

    cmd = args[0]

    if cmd == "add-token":
        if len(args) < 2:
            print("usage: add-token <label>", file=sys.stderr)
            sys.exit(1)
        cmd_add_token(db, args[1])

    elif cmd == "add-workflow":
        if len(args) < 3:
            print("usage: add-workflow <uri> <blob_path>", file=sys.stderr)
            sys.exit(1)
        cmd_add_workflow(db, blobs_dir, args[1], args[2])

    elif cmd == "add-update":
        if len(args) < 5:
            print("usage: add-update <channel> <arch> <version> <bin_path>", file=sys.stderr)
            sys.exit(1)
        cmd_add_update(db, blobs_dir, args[1], args[2], args[3], args[4])

    elif cmd == "list-workflows":
        cmd_list_workflows(db)

    else:
        print(f"unknown command: {cmd}", file=sys.stderr)
        usage()


if __name__ == "__main__":
    main()
