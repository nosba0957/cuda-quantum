#!/usr/bin/env bash
# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #
# Regenerate tests/corpus/*.qasm from tests/corpus/quake/*.qke using the real
# nvq++ tools. build/bin/* needs a newer GLIBC than the host, so run them in the
# devcontainer image.
set -euo pipefail
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
IMAGE=${IMAGE:-ghcr.io/nvidia/cuda-quantum-devcontainer:cu12.6-gcc12-main}
REL=runtime/cudaq/platform/default/rest/helpers/quantum_machines/tests/corpus

docker run --rm -v "$REPO:$REPO" -w "$REPO" "$IMAGE" bash -lc '
set -euo pipefail
D='"$REL"'
# The quantum_machines target'"'"'s jit-mid-level-pipeline, verbatim from
# quantum_machines.yml -- this is what the runtime applies before qasm2 codegen.
P="builtin.module(lower-to-cfg,decomposition{basis=h,s,t,r1,rx,ry,rz,x,y,z,x(1)},quake-to-cc-prep,func.func(expand-control-veqs,combine-quantum-alloc,canonicalize,combine-measurements))"
for f in bell rz_sweep sx_chain qaoa_p1; do
  build/bin/cudaq-opt --pass-pipeline="$P" $D/quake/$f.qke \
    | build/bin/cudaq-translate --convert-to=openqasm2 > $D/$f.qasm
done
# cz survives only without the decomposition pass; with it, quake.z[ctrl]
# becomes h/cx/h.  Kept native so the corpus exercises the transpiler cz path.
Q="builtin.module(lower-to-cfg,quake-to-cc-prep,func.func(expand-control-veqs,combine-quantum-alloc,canonicalize,combine-measurements))"
build/bin/cudaq-opt --pass-pipeline="$Q" $D/quake/cz_pair.qke \
  | build/bin/cudaq-translate --convert-to=openqasm2 > $D/cz_pair.qasm
chown '"$(id -u):$(id -g)"' $D/*.qasm
'
