#!/bin/bash
# Install superrouter as a systemd service on a Linux host.
# Run as root.

set -euo pipefail

PORT="${SUPERROUTER_PORT:-8420}"
DB="${SUPERROUTER_DB:-/var/lib/anunix-superrouter/db.sqlite3}"
ADMIN_SECRET="${SUPERROUTER_ADMIN_SECRET:-}"
INSTALL_DIR="/opt/anunix-superrouter"
SERVICE_USER="anunix-sr"

if [[ $EUID -ne 0 ]]; then
    echo "Run as root." >&2
    exit 1
fi

echo "Installing superrouter to ${INSTALL_DIR}..."

mkdir -p "${INSTALL_DIR}"
cp "$(dirname "$0")/server.py" "${INSTALL_DIR}/server.py"
cp "$(dirname "$0")/schema.sql" "${INSTALL_DIR}/schema.sql"
chmod 755 "${INSTALL_DIR}/server.py"

id "${SERVICE_USER}" &>/dev/null || useradd -r -s /bin/false "${SERVICE_USER}"
mkdir -p "$(dirname "${DB}")"
chown "${SERVICE_USER}:" "$(dirname "${DB}")"

cat > /etc/systemd/system/anunix-superrouter.service <<EOF
[Unit]
Description=Anunix superrouter — hardware profile library and driver stub registry
After=network.target

[Service]
Type=simple
User=${SERVICE_USER}
ExecStart=/usr/bin/python3 ${INSTALL_DIR}/server.py --port ${PORT} --db ${DB}
Environment=SUPERROUTER_ADMIN_SECRET=${ADMIN_SECRET}
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable anunix-superrouter
systemctl restart anunix-superrouter

echo "superrouter installed and running on port ${PORT}."
echo "Provision a node token:"
echo "  curl -s -X POST http://localhost:${PORT}/v1/admin/tokens \\"
echo "       -H 'Authorization: Bearer \${SUPERROUTER_ADMIN_SECRET}' \\"
echo "       -H 'Content-Type: application/json' \\"
echo "       -d '{\"node_id\": \"<node-uuid>\", \"hostname\": \"<hostname>\"}'"
