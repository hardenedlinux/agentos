#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${AGENTOS_HOME:-$HOME/.agentos}"
mkdir -p "$INSTALL_DIR/bin"
cp bin/agentos bin/uv "$INSTALL_DIR/bin/"
chmod +x "$INSTALL_DIR/bin/agentos" "$INSTALL_DIR/bin/uv"

mkdir -p "$HOME/.config/systemd/user"
cp agentos.service "$HOME/.config/systemd/user/"   #  ADR-014 

systemctl --user daemon-reload
systemctl --user enable --now agentos
