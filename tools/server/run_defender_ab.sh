#!/usr/bin/env bash
# Independent checkpoints per leg; rerun this exact command after a restart.
# Default pilot: 200k then 1M per leg, evaluate at each stage before spending 40M.
set -euo pipefail
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$REPO"
PY=${RTS_PY:-/data0/am_data/miniforge3/bin/python}
BIND=${RTS_BINDINGS_DIR:-$REPO/build-Release/bindings}
OUT=${RTS_RUN_ROOT:-$REPO/runs/defender-ab}
if [ "$#" -gt 0 ] && [[ "$1" =~ ^[0-9]+$ ]]; then
    LEGACY_STEPS=$1
    shift
    set -- --milestones "$LEGACY_STEPS" "$@"
fi
[ -d "$BIND" ] || { echo "Build bindings first: RTS_BINDINGS=1 tools/server/build.sh Release" >&2; exit 2; }
if command -v nvidia-smi >/dev/null 2>&1; then
    nvidia-smi --query-gpu=memory.used,memory.total,utilization.gpu --format=csv,noheader
fi
export PYTHONPATH="$BIND${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONIOENCODING=utf-8 PYTHONUNBUFFERED=1
exec "$PY" train/experiments.py --run-root "$OUT" "$@"
