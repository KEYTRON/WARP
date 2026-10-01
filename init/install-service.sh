#!/bin/sh
# WARP init service installer — auto-detects systemd / OpenRC / runit
set -e

ARG="${1:-seed}"      # seed or volunteer; both install the one node service, warp-seed
SERVICE=seed
WARP_BIN="/usr/local/bin/warp"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

die() { echo "ERROR: $*" >&2; exit 1; }
ok()  { echo "  ✓ $*"; }
info(){ echo "  → $*"; }

# ── macOS: a per-user launchd agent, no root; the node reads its port from the config ──
if [ "$(uname -s)" = Darwin ]; then
    WARP_BIN=/opt/warp/bin/warp
    [ -x "$WARP_BIN" ] || die "warp not found at $WARP_BIN. Run the installer first."
    if [ "$ARG" = "volunteer" ]; then "$WARP_BIN" volunteer --setup --no-start; fi
    LABEL=dev.keytron.warp-seed
    PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
    mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
    cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>$WARP_BIN</string><string>seed</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$HOME/Library/Logs/warp-seed.log</string>
  <key>StandardErrorPath</key><string>$HOME/Library/Logs/warp-seed.log</string>
</dict></plist>
PL
    launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
    launchctl bootstrap "gui/$(id -u)" "$PLIST"
    ok "warp-seed started at login (launchd agent $PLIST)"
    info "Log: ~/Library/Logs/warp-seed.log; stop: launchctl bootout gui/$(id -u)/$LABEL"
    exit 0
fi

[ -x "$WARP_BIN" ] || die "warp not found at $WARP_BIN. Run 'make install' first."
[ "$ARG" = "seed" ] || [ "$ARG" = "volunteer" ] || \
    die "Usage: $0 [seed|volunteer]"

# One process does both jobs: volunteer mode is a setting, not a second service.
if [ "$ARG" = "volunteer" ]; then
    echo ""
    echo "  Volunteer mode is a setting of the seed service. Running setup..."
    warp volunteer --setup --no-start
fi

echo ""
echo "  Installing warp-$SERVICE service..."
echo ""

# ── systemd ──────────────────────────────────────────────────────────
if command -v systemctl >/dev/null 2>&1 && [ -d /etc/systemd/system ]; then
    info "Detected: systemd"
    cp "$SCRIPT_DIR/warp-$SERVICE.service" /etc/systemd/system/
    systemctl daemon-reload
    systemctl enable "warp-$SERVICE"
    systemctl start  "warp-$SERVICE"
    ok "warp-$SERVICE enabled and started (systemd)"
    systemctl status "warp-$SERVICE" --no-pager -l | head -10
    exit 0
fi

# ── OpenRC ───────────────────────────────────────────────────────────
if command -v rc-update >/dev/null 2>&1 && [ -d /etc/init.d ]; then
    info "Detected: OpenRC"
    cp "$SCRIPT_DIR/warp-$SERVICE.openrc" "/etc/init.d/warp-$SERVICE"
    chmod +x "/etc/init.d/warp-$SERVICE"
    rc-update add "warp-$SERVICE" default
    rc-service "warp-$SERVICE" start
    ok "warp-$SERVICE enabled and started (OpenRC)"
    exit 0
fi

# ── runit ────────────────────────────────────────────────────────────
if command -v sv >/dev/null 2>&1; then
    SV_DIR=""
    for d in /etc/sv /service /var/service; do
        [ -d "$d" ] && SV_DIR="$d" && break
    done

    if [ -n "$SV_DIR" ]; then
        info "Detected: runit (sv dir: $SV_DIR)"
        cp -r "$SCRIPT_DIR/runit/warp-$SERVICE" "$SV_DIR/"
        chmod +x "$SV_DIR/warp-$SERVICE/run"

        # Void Linux: link into /var/service to enable
        if [ -d /var/service ] && [ "$SV_DIR" != "/var/service" ]; then
            ln -sf "$SV_DIR/warp-$SERVICE" /var/service/
        fi

        sv start "warp-$SERVICE"
        ok "warp-$SERVICE enabled and started (runit)"
        exit 0
    fi
fi

# ── s6 ───────────────────────────────────────────────────────────────
if command -v s6-rc >/dev/null 2>&1 || [ -d /etc/s6 ]; then
    info "Detected: s6 — creating oneshot service"
    mkdir -p "/etc/s6/warp-$SERVICE"
    printf '#!/execlineb -P\n/usr/local/bin/warp %s\n' "$SERVICE" \
        > "/etc/s6/warp-$SERVICE/run"
    chmod +x "/etc/s6/warp-$SERVICE/run"
    ok "s6 service created at /etc/s6/warp-$SERVICE"
    info "Run: s6-rc-compile && s6-rc -u change warp-$SERVICE"
    exit 0
fi

die "No supported init system found (tried systemd, OpenRC, runit, s6)"
