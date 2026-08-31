#!/usr/bin/env python3
# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

"""QuAM state -> serialized QuaConfig, the opaque blob the C++ side sends as
the `config` field of QmmService/OpenQuantumMachine."""

import argparse
import json
import os
import sys
from pathlib import Path

from qm.api.models.capabilities import QopCaps, ServerCapabilities, offline_capabilities
from qm.program._dict_to_pb_converter import DictToQuaConfigConverter
from qm.program._qua_config_schema import load_config, validate_config_capabilities


def sdk_version():
    import qm
    return getattr(qm, "__version__", "unknown")


def caps_from_names(names):
    by_name = {c.qop_name: c for c in QopCaps.get_all()}
    unknown = sorted(set(names) - set(by_name))
    if unknown:
        raise SystemExit(f"qm {sdk_version()} does not know capabilities {unknown}")
    return ServerCapabilities([by_name[n] for n in names])


def caps_from_server(host, port, cluster_name, timeout):
    from qm import QuantumMachinesManager

    kwargs = dict(host=host, cluster_name=cluster_name, timeout=timeout)
    if port is not None:
        kwargs["port"] = port
    qmm = QuantumMachinesManager(**kwargs)
    v = qmm.version()
    return qmm.capabilities, {
        "QOP": getattr(v, "QOP", None),
        "gateway": getattr(v, "gateway", None),
        "qm_qua": getattr(v, "qm_qua", None),
    }


def load_machine(state_path):
    if state_path:
        os.environ["QUAM_STATE_PATH"] = str(state_path)
    if "QUAM_STATE_PATH" not in os.environ:
        raise SystemExit("pass --state-path or set $QUAM_STATE_PATH")
    from quam_libs.components import QuAM
    return QuAM.load()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--state-path", default=None,
                    help="QuAM state directory (default: $QUAM_STATE_PATH)")
    ap.add_argument("--caps-from", choices=["server", "file", "all"], default="server",
                    help="DictToQuaConfigConverter is parameterized by server "
                    "capabilities and emits a structurally different message "
                    "without them -- qm.config_v2 alone switches the top-level "
                    "wrapper between v1beta and v2 -- so 'server' (default) is "
                    "the only mode that guarantees a wire format the QOP expects")
    ap.add_argument("--caps-file", default=None)
    ap.add_argument("--host", default=None,
                    help="default: the host in the QuAM network block")
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--cluster-name", default=None)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--no-marshmallow", action="store_true",
                    help="skip schema validation, as open_qm(validate_with_protobuf=True)")
    ap.add_argument("--text", default=None, help="also write the text form here")
    ap.add_argument("--json", default=None, help="also write generate_config() here")
    args = ap.parse_args(argv)

    machine = load_machine(args.state_path)
    network = getattr(machine, "network", None) or {}

    version_info = None
    if args.caps_from == "server":
        host = args.host or network.get("host")
        port = args.port if args.port is not None else network.get("port")
        cluster = args.cluster_name or network.get("cluster_name")
        if not host:
            raise SystemExit("no QOP host: pass --host or give the state a network block")
        caps, version_info = caps_from_server(host, port, cluster, args.timeout)
        names = sorted(c.qop_name for c in caps.supported_capabilities)
    elif args.caps_from == "file":
        if not args.caps_file:
            raise SystemExit("--caps-from file requires --caps-file")
        payload = json.loads(Path(args.caps_file).read_text())
        names = sorted(payload["capabilities"])
        caps = caps_from_names(names)
        version_info = payload.get("qop_version")
    else:
        caps = ServerCapabilities(QopCaps.get_all())
        names = sorted(c.qop_name for c in caps.supported_capabilities)

    offline_capabilities.set(caps.supported_capabilities)

    config = machine.generate_config()
    if args.json:
        Path(args.json).write_text(json.dumps(config, indent=2, default=repr))

    pb = (DictToQuaConfigConverter(caps).convert(config)
          if args.no_marshmallow else load_config(config, capabilities=caps))
    validate_config_capabilities(pb, caps)

    blob = pb.SerializeToString()
    out = Path(args.output)
    out.write_bytes(blob)

    sidecar = out.with_name(out.name + ".caps.json")
    sidecar.write_text(
        json.dumps(
            {
                "capabilities": names,
                "caps_from": args.caps_from,
                "qop_version": version_info,
                "qm_qua": sdk_version(),
                "state_path": os.environ.get("QUAM_STATE_PATH"),
                "config_wrapper": pb.WhichOneof("config_version"),
                "bytes": len(blob),
            },
            indent=2) + "\n")

    if args.text:
        Path(args.text).write_text(str(pb))

    print(f"wrote {out} ({len(blob)} bytes, wrapper "
          f"{pb.WhichOneof('config_version')}) and {sidecar} "
          f"({len(names)} capabilities)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
