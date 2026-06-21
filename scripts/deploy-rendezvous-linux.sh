#!/usr/bin/env bash
# Установка nyx-rendezvous на Linux VDS
set -euo pipefail

INSTALL_DIR="/opt/nyx"
BIND="0.0.0.0:3478"
RATE_LIMIT=120

while [[ $# -gt 0 ]]; do
  case "$1" in
    --install) INSTALL_DIR="$2"; shift 2 ;;
    --bind) BIND="$2"; shift 2 ;;
    --rate-limit) RATE_LIMIT="$2"; shift 2 ;;
    *) echo "Unknown: $1"; exit 1 ;;
  esac
done

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cmake -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" --target nyx-rendezvous -j"$(nproc)"

sudo install -d "$INSTALL_DIR"
sudo install -m755 "$ROOT/build/nyx-rendezvous" "$INSTALL_DIR/"

UNIT="/etc/systemd/system/nyx-rendezvous.service"
sudo tee "$UNIT" >/dev/null <<EOF
[Unit]
Description=Nyx rendezvous bootstrap
After=network-online.target

[Service]
Type=simple
ExecStart=$INSTALL_DIR/nyx-rendezvous --bind=$BIND --rate-limit=$RATE_LIMIT
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now nyx-rendezvous
echo "nyx-rendezvous installed at $INSTALL_DIR (UDP $BIND)"
