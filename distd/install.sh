#!/bin/bash
# Install anunix-distd as a systemd service on a Linux host.
# Run as root.

set -euo pipefail

PORT="${DISTD_PORT:-8420}"
DB="${DISTD_DB:-/var/lib/anunix-distd/db.sqlite3}"
BLOBS="${DISTD_BLOBS:-/var/lib/anunix-distd/blobs}"
INSTALL_DIR="/opt/anunix-distd"
SERVICE_USER="anunix-distd"

if [[ $EUID -ne 0 ]]; then
    echo "Run as root." >&2
    exit 1
fi

echo "Installing anunix-distd to ${INSTALL_DIR}..."

mkdir -p "${INSTALL_DIR}"
cp "$(dirname "$0")/server.py" "${INSTALL_DIR}/server.py"
cp "$(dirname "$0")/manage.py" "${INSTALL_DIR}/manage.py"
chmod 755 "${INSTALL_DIR}/server.py" "${INSTALL_DIR}/manage.py"

id "${SERVICE_USER}" &>/dev/null || useradd -r -s /bin/false "${SERVICE_USER}"
mkdir -p "$(dirname "${DB}")" "${BLOBS}"
chown "${SERVICE_USER}:" "$(dirname "${DB}")" "${BLOBS}"

cat > /etc/systemd/system/anunix-distd.service <<EOF
[Unit]
Description=Anunix Distribution Services
After=network.target

[Service]
Type=simple
User=${SERVICE_USER}
ExecStart=/usr/bin/python3 ${INSTALL_DIR}/server.py --port ${PORT} --db ${DB} --blobs ${BLOBS}
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable anunix-distd
systemctl restart anunix-distd

echo "anunix-distd installed and running on port ${PORT}."
echo "Create a first token (bootstrap — no auth required when table is empty):"
echo "  curl -s -X POST http://localhost:${PORT}/tokens/ | python3 -m json.tool"
