#!/usr/bin/env python3
"""
anxbproxy.py — HTTP CONNECT proxy for Anunix Browser HTTPS support.

The Anunix kernel browser driver does not implement TLS.  HTTPS requests
are tunnelled through this proxy:

  Kernel → TCP CONNECT host:443 → anxbproxy → TLS → remote server

Usage:
    python3 tools/anxbproxy.py [--host 0.0.0.0] [--port 8118]

QEMU user-mode networking forwards guest port to host automatically;
the kernel reaches the proxy at 10.0.2.2:8118 (QEMU host alias).

For bare-metal / UTM use, pass --host 0.0.0.0 and configure the guest
to reach the host machine's IP on port 8118.
"""

import argparse
import socket
import ssl
import threading
import sys

CONNECT_TIMEOUT = 15   # seconds to establish upstream connection
IO_TIMEOUT      = 30   # seconds per recv() on established tunnels
BUF_SIZE        = 65536


def pipe(src: socket.socket, dst: socket.socket, label: str) -> None:
    """Forward bytes from src to dst until src closes or errors."""
    try:
        while True:
            data = src.recv(BUF_SIZE)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    finally:
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass


def handle_connect(client: socket.socket, host: str, port: int) -> None:
    """Open a TLS connection to host:port and splice it with the client."""
    ctx = ssl.create_default_context()
    raw = socket.create_connection((host, port), timeout=CONNECT_TIMEOUT)
    upstream = ctx.wrap_socket(raw, server_hostname=host)
    upstream.settimeout(IO_TIMEOUT)

    # Tell the kernel the tunnel is open.
    client.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")

    # Splice in both directions concurrently.
    t = threading.Thread(target=pipe, args=(upstream, client, "up→client"),
                         daemon=True)
    t.start()
    pipe(client, upstream, "client→up")
    t.join(timeout=IO_TIMEOUT + 5)


def handle_client(conn: socket.socket, addr) -> None:
    conn.settimeout(IO_TIMEOUT)
    try:
        # Read the request line (CONNECT host:port HTTP/1.1\r\n).
        request = b""
        while b"\r\n\r\n" not in request:
            chunk = conn.recv(4096)
            if not chunk:
                return
            request += chunk

        first_line = request.split(b"\r\n")[0].decode(errors="replace")
        parts = first_line.split()
        if len(parts) < 3 or parts[0].upper() != "CONNECT":
            conn.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")
            return

        # Parse "host:port"
        target = parts[1]
        if b":" in target:
            host_b, port_b = target.rsplit(b":", 1)
            host = host_b.decode()
            port = int(port_b)
        else:
            host = target.decode()
            port = 443

        print(f"[anxbproxy] CONNECT {host}:{port} from {addr[0]}", flush=True)
        handle_connect(conn, host, port)

    except (OSError, ssl.SSLError, ValueError) as exc:
        print(f"[anxbproxy] error from {addr[0]}: {exc}", flush=True)
        try:
            conn.sendall(b"HTTP/1.1 502 Bad Gateway\r\n\r\n")
        except OSError:
            pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main() -> None:
    parser = argparse.ArgumentParser(description="Anunix Browser HTTPS proxy")
    parser.add_argument("--host", default="0.0.0.0",
                        help="Bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8118,
                        help="Bind port (default: 8118)")
    args = parser.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(64)
    print(f"[anxbproxy] listening on {args.host}:{args.port}", flush=True)

    try:
        while True:
            conn, addr = srv.accept()
            t = threading.Thread(target=handle_client, args=(conn, addr),
                                 daemon=True)
            t.start()
    except KeyboardInterrupt:
        print("\n[anxbproxy] shutting down", flush=True)
    finally:
        srv.close()


if __name__ == "__main__":
    main()
