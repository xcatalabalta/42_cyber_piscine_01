#!/usr/bin/env bash
#
# scorpion.sh - self-contained launcher for the library (Pillow) edition.
#
# On first run it creates a local virtual environment next to this script and
# installs requirements.txt. On later runs it reinstalls ONLY if requirements.txt
# has changed since the last install (tracked by a hash stamp); otherwise it goes
# straight to execution. All setup chatter goes to stderr so stdout stays clean.
#
#   ./scorpion.sh FILE1 [FILE2 ...]
#
set -euo pipefail

# Absolute directory of THIS script (also resolves a symlinked launcher),
# so it works from any current directory.
SOURCE="${BASH_SOURCE[0]}"
while [ -L "$SOURCE" ]; do
    DIR="$(cd -P "$(dirname "$SOURCE")" && pwd)"
    SOURCE="$(readlink "$SOURCE")"
    [[ "$SOURCE" != /* ]] && SOURCE="$DIR/$SOURCE"
done
SCRIPT_DIR="$(cd -P "$(dirname "$SOURCE")" && pwd)"

VENV_DIR="$SCRIPT_DIR/.venv"
PY_PROG="$SCRIPT_DIR/scorpion.py"
REQ="$SCRIPT_DIR/requirements.txt"
STAMP="$VENV_DIR/.requirements.sha"

# Create the venv once.
if [ ! -x "$VENV_DIR/bin/python" ]; then
    echo "[scorpion] creating virtual environment..." >&2
    python3 -m venv "$VENV_DIR"
    "$VENV_DIR/bin/pip" install --quiet --upgrade pip >&2
fi

# (Re)install deps if requirements.txt is new or has changed since last install.
if [ -f "$REQ" ]; then
    want="$(sha256sum "$REQ" | awk '{print $1}')"
    have="$(cat "$STAMP" 2>/dev/null || true)"
    if [ "$want" != "$have" ]; then
        echo "[scorpion] installing/updating dependencies..." >&2
        "$VENV_DIR/bin/pip" install --quiet -r "$REQ" >&2
        echo "$want" > "$STAMP"
        echo "[scorpion] environment ready." >&2
    fi
fi

# exec: replace the shell so exit code and signals pass straight through.
exec "$VENV_DIR/bin/python" "$PY_PROG" "$@"