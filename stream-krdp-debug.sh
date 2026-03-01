#!/usr/bin/env bash
# Stream KRDP server debug logs (e.g. swipe events).
#
# The service normally runs the system-installed krdpserver. To run YOUR
# built binary and see your logs, use one of:
#
#   ./stream-krdp-debug.sh --use-binary PATH   # point service at your build (once)
#   ./stream-krdp-debug.sh --foreground PATH   # run in terminal, no systemd
#
# Then rebuild (ninja -C build) and run ./stream-krdp-debug.sh as usual.
# The built binary is at build/bin/krdpserver (not build/server/).
#
# Options:
#   --use-binary PATH   Make the service use this krdpserver binary (creates a
#                       systemd drop-in). PATH can be relative, e.g. build/bin/krdpserver
#   --foreground [-f] [PATH]  Stop service and run krdpserver in this terminal.
#   (default)            Reload service and follow journalctl.
set -e

SERVICE="app-org.kde.krdpserver.service"
USER_UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
OVERRIDE_DIR="$USER_UNIT_DIR/$SERVICE.d"
OVERRIDE_FILE="$OVERRIDE_DIR/debug.conf"
EXEC_OVERRIDE_FILE="$OVERRIDE_DIR/exec.conf"

enable_debug() {
    if [[ -f "$OVERRIDE_FILE" ]]; then
        return 0
    fi
    echo "Enabling KRDP debug logging for $SERVICE..."
    mkdir -p "$OVERRIDE_DIR"
    cat > "$OVERRIDE_FILE" << 'EOF'
[Service]
Environment="QT_LOGGING_RULES=krdp.*=true"
EOF
    systemctl --user daemon-reload
    systemctl --user restart "$SERVICE"
    echo "Service restarted with debug enabled."
}

if [[ "${1:-}" == "--use-binary" ]]; then
    if [[ -z "${2:-}" ]]; then
        echo "Usage: $0 --use-binary PATH_TO_KRDPSERVER"
        echo "Example: $0 --use-binary $(pwd)/build/bin/krdpserver"
        exit 1
    fi
    BINARY="$2"
    if [[ "$BINARY" != /* ]]; then
        BINARY="$(pwd)/$BINARY"
    fi
    if [[ ! -x "$BINARY" ]]; then
        echo "Error: not executable: $BINARY"
        echo "Build the project first (e.g. ninja -C build) so that the binary exists."
        exit 1
    fi
    echo "Pointing $SERVICE at your binary: $BINARY"
    mkdir -p "$OVERRIDE_DIR"
    echo "[Service]" > "$EXEC_OVERRIDE_FILE"
    echo "ExecStart=$BINARY" >> "$EXEC_OVERRIDE_FILE"
    systemctl --user daemon-reload
    systemctl --user restart "$SERVICE"
    echo "Done. The service now uses your build. Rebuild with 'ninja -C build' (or your build dir) when you change code."
    echo "Stream logs with: $0"
    exit 0
fi

if [[ "${1:-}" == "--foreground" || "${1:-}" == "-f" ]]; then
    echo "Stopping $SERVICE so we can run the server in the foreground..."
    systemctl --user stop "$SERVICE" 2>/dev/null || true
    BINARY="${2:-krdpserver}"
    if [[ -n "${2:-}" && ! -x "$BINARY" ]]; then
        echo "Error: not executable: $BINARY"
        exit 1
    fi
    echo "Running: $BINARY (logs will appear below; Ctrl+C to stop)"
    echo "Start the service again later with: systemctl --user start $SERVICE"
    export QT_LOGGING_RULES="krdp.*=true"
    exec "$BINARY"
fi

echo "Reloading KRDP..."
systemctl --user daemon-reload
systemctl --user restart "$SERVICE"
enable_debug
exec journalctl --user -u "$SERVICE" -f
