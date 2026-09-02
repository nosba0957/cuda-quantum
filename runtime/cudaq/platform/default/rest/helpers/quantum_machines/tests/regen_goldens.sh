#!/usr/bin/env bash
# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #
# Rebuild qua_config.pb and every corpus golden. Both depend on the QuAM state,
# so a recalibration moves them: the CZ phase compensations are baked into the
# QuaProgram, not just into the config.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PY=${PY:-/home/asrlabncku/as_ntu_ncku/.venv/bin/python}
STATE=${QUAM_STATE_PATH:-$HOME/as_ntu_ncku/as-qpu-10q9c}

"$PY" "$HERE/../tools/quam2pb.py" -o "$HERE/qua_config.pb" --state-path "$STATE"
for f in "$HERE"/corpus/*.qasm; do
  n=$(basename "$f" .qasm)
  "$PY" "$HERE/../tools/qua_build.py" --qasm "$f" \
    --caps-file "$HERE/qua_config.pb.caps.json" --state-path "$STATE" \
    -o "$HERE/corpus/$n.golden.pb" --manifest "$HERE/corpus/$n.manifest.json"
done
