#!/usr/bin/env python3
# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #
"""A mock QOP gateway speaking the seven RPCs the CUDA-Q executor calls.

Built on the qm SDK's own descriptors, so a request the mock accepts is a
request the real QOP would parse. Every call is appended to a JSON-lines RPC
log that tests assert against. Needs the venv python that has qm installed.

    mock_qop.py --listen 127.0.0.1:9515 --log /tmp/rpc.jsonl \
                --stream-frames 3 --stream-delay 0.4 --results 00,11,11,00
"""

import argparse
import hashlib
import json
import struct
import sys
import threading
import time
from concurrent import futures

import grpc
from qm.grpc.qm.grpc.v2 import common_types_pb2, job_api_pb2, qm_api_pb2, qmm_api_pb2
from qm.grpc.qm.pb import inc_qua_pb2

QMM = "/qm.grpc.v2.QmmService"
QM = "/qm.grpc.v2.QmService"
JOB = "/qm.grpc.v2.JobService"


def _digest(blob):
    return hashlib.sha256(blob).hexdigest()[:16]


class MockQOP:

    def __init__(self, args):
        self.args = args
        self.log = []
        self.lock = threading.Lock()
        self.counter = 0
        self.pending_polls = {}
        self.log_file = open(args.log, "w") if args.log else None

    def record(self, method, payload, **fields):
        with self.lock:
            self.counter += 1
            entry = {
                "seq": self.counter,
                "method": method,
                "bytes": len(payload),
                **fields
            }
            self.log.append(entry)
            if self.log_file:
                self.log_file.write(json.dumps(entry) + "\n")
                self.log_file.flush()
        if self.args.verbose:
            print(json.dumps(entry), file=sys.stderr, flush=True)
        return entry

    # -- handlers ------------------------------------------------------------

    def get_version(self, payload, context):
        self.record(f"{QMM}/GetVersion", payload)
        resp = qmm_api_pb2.GetVersionResponse()
        resp.success.gateway = self.args.gateway_version
        resp.success.controllers["con1"] = self.args.gateway_version
        return resp.SerializeToString()

    def open_quantum_machine(self, payload, context):
        req = qmm_api_pb2.OpenQuantumMachineRequest.FromString(payload)
        # config is a QuaConfig in the real schema, so parsing the request has
        # already validated the blob the C++ side shipped as opaque bytes. Its
        # re-serialized length is logged but not its digest: QuaConfig has maps,
        # and map ordering is not stable across serializations.
        self.record(f"{QMM}/OpenQuantumMachine",
                    payload,
                    config_bytes=len(req.config.SerializeToString()),
                    close_mode=req.close_mode)
        resp = qmm_api_pb2.OpenQuantumMachineResponse()
        resp.success.quantum_machine_id = self.args.machine_id
        return resp.SerializeToString()

    def compile(self, payload, context):
        req = qm_api_pb2.QmServiceCompileRequest.FromString(payload)
        program_bytes = req.high_level_program.SerializeToString()
        prog = inc_qua_pb2.QuaProgram.FromString(program_bytes)
        streams = sorted(
            v.name for v in prog.script.variables if v.isInputStream)
        program_id = f"{self.args.program_id_prefix}{_digest(program_bytes)}"
        self.record(f"{QM}/Compile",
                    payload,
                    quantum_machine_id=req.quantum_machine_id,
                    program_bytes=len(program_bytes),
                    program_digest=_digest(program_bytes),
                    input_streams=streams,
                    has_config=req.HasField("config"),
                    program_id=program_id)
        resp = qm_api_pb2.CompileResponse()
        resp.success.program_id = program_id
        return resp.SerializeToString()

    def add_compiled_to_queue(self, payload, context):
        req = qm_api_pb2.QmServiceAddCompiledToQueueRequest.FromString(payload)
        job_id = f"{self.args.job_id_prefix}{req.program_id[-8:]}"
        self.record(f"{QM}/AddCompiledToQueue",
                    payload,
                    quantum_machine_id=req.quantum_machine_id,
                    program_id=req.program_id,
                    job_id=job_id)
        resp = qm_api_pb2.AddCompiledToQueueResponse()
        resp.success.job_id = job_id
        return resp.SerializeToString()

    def get_job_status(self, payload, context):
        req = job_api_pb2.JobServiceGetJobStatusRequest.FromString(payload)
        seen = self.pending_polls.get(req.job_id, 0)
        self.pending_polls[req.job_id] = seen + 1
        # Real hardware queues; report PENDING first so the executor's wait
        # loop is exercised rather than short-circuited.
        running = seen >= self.args.pending_polls
        self.record(f"{JOB}/GetJobStatus",
                    payload,
                    job_id=req.job_id,
                    poll=seen,
                    status="RUNNING" if running else "PENDING")
        resp = job_api_pb2.JobServiceGetJobStatusResponse()
        resp.success.status = (common_types_pb2.RUNNING
                               if running else common_types_pb2.PENDING)
        return resp.SerializeToString()

    def close(self, payload, context):
        req = qm_api_pb2.QmServiceCloseRequest.FromString(payload)
        self.record(f"{QM}/Close",
                    payload,
                    quantum_machine_id=req.quantum_machine_id)
        resp = qm_api_pb2.QmServiceCloseResponse()
        resp.success.SetInParent()
        return resp.SerializeToString()

    def push_to_input_stream(self, payload, context):
        req = job_api_pb2.JobServicePushToInputStreamRequest.FromString(payload)
        kind = req.WhichOneof("stream_data_oneof")
        values = list(getattr(req, kind).data) if kind else []
        self.record(f"{JOB}/PushToInputStream",
                    payload,
                    job_id=req.job_id,
                    stream_name=req.stream_name,
                    data_kind=kind,
                    values=values)
        resp = job_api_pb2.PushToInputStreamResponse()
        resp.success.SetInParent()
        return resp.SerializeToString()

    def get_named_results(self, payload, context):
        req = job_api_pb2.GetNamedResultsRequest.FromString(payload)
        names = [o.output_name for o in req.outputs]
        name = names[0] if names else self.args.creg_name
        chunks = self._result_chunks()
        self.record(f"{JOB}/GetNamedResults",
                    payload,
                    job_id=req.job_id,
                    outputs=names,
                    ranges=[[getattr(o.range, "from").value, o.range.to.value]
                            if o.HasField("range") else None
                            for o in req.outputs],
                    frames=len(chunks) + 1,
                    buffers=len(self.args.results))

        for i, chunk in enumerate(chunks):
            if i:
                time.sleep(self.args.stream_delay)
            resp = job_api_pb2.GetNamedResultResponse()
            resp.success.output_name = name
            resp.success.data_chunk.data = chunk
            yield resp.SerializeToString()

        time.sleep(self.args.stream_delay)
        resp = job_api_pb2.GetNamedResultResponse()
        resp.success.output_name = name
        resp.success.data_summary.count = len(self.args.results)
        yield resp.SerializeToString()

    def _result_chunks(self):
        """Pack the configured shot outcomes into --stream-frames DataChunks.

        Each shot is a length-`creg_size` array of int64, element i being clbit
        i, matching `boolean_to_int().buffer(n).save_all(name)`. Chunk
        boundaries deliberately do not align to shot boundaries: the stream
        carries a byte range, not framed records.
        """
        blob = b""
        for bits in self.args.results:
            blob += struct.pack(
                f"<{self.args.creg_size}q",
                *[(bits >> i) & 1 for i in range(self.args.creg_size)])
        n = max(1, self.args.stream_frames)
        size = (len(blob) + n - 1) // n
        return [blob[i:i + size] for i in range(0, len(blob), size)] or [b""]

    # -- grpc plumbing -------------------------------------------------------

    def handlers(self):
        unary = grpc.unary_unary_rpc_method_handler
        stream = grpc.unary_stream_rpc_method_handler
        ident = (lambda b: b, lambda b: b)
        return {
            f"{QMM}/GetVersion":
                unary(self.get_version, *ident),
            f"{QMM}/OpenQuantumMachine":
                unary(self.open_quantum_machine, *ident),
            f"{QM}/Compile":
                unary(self.compile, *ident),
            f"{QM}/AddCompiledToQueue":
                unary(self.add_compiled_to_queue, *ident),
            f"{QM}/Close":
                unary(self.close, *ident),
            f"{JOB}/GetJobStatus":
                unary(self.get_job_status, *ident),
            f"{JOB}/PushToInputStream":
                unary(self.push_to_input_stream, *ident),
            f"{JOB}/GetNamedResults":
                stream(self.get_named_results, *ident),
        }


class _Router(grpc.GenericRpcHandler):

    def __init__(self, table):
        self.table = table

    def service(self, handler_call_details):
        return self.table.get(handler_call_details.method)


def _bitstring(s):
    """LSB-first: '01' means clbit 0 = 0, clbit 1 = 1."""
    return sum(int(c) << i for i, c in enumerate(s))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="127.0.0.1:9515")
    ap.add_argument("--log", help="append each RPC to this file as JSON lines")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--stream-frames",
                    type=int,
                    default=3,
                    help="DataChunk frames GetNamedResults splits results into")
    ap.add_argument("--stream-delay",
                    type=float,
                    default=0.4,
                    help="seconds between streamed frames")
    ap.add_argument("--pending-polls", type=int, default=1,
                    help="GetJobStatus replies PENDING this many times before "
                         "RUNNING, modelling the real queue")
    ap.add_argument("--results",
                    default="00,11,11,00",
                    help="comma-separated per-shot outcomes, LSB-first")
    ap.add_argument("--creg-name", default="var3")
    ap.add_argument("--machine-id", default="mock-qm-1")
    ap.add_argument("--program-id-prefix", default="mock-prog-")
    ap.add_argument("--job-id-prefix", default="mock-job-")
    ap.add_argument("--gateway-version", default="3.6.0-mock")
    ap.add_argument("--ready-file", help="touch this once the port is bound")
    args = ap.parse_args()

    strings = [s.strip() for s in args.results.split(",") if s.strip()]
    args.creg_size = len(strings[0]) if strings else 1
    if any(len(s) != args.creg_size for s in strings):
        raise SystemExit("--results entries must all be the same width")
    args.results = [_bitstring(s) for s in strings]

    mock = MockQOP(args)
    # Without this grpc sets SO_REUSEPORT, so a stale mock on the same port
    # silently shares it and half the RPCs never reach the log a test asserts on.
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=8),
                         options=[("grpc.so_reuseport", 0)])
    server.add_generic_rpc_handlers((_Router(mock.handlers()),))
    if server.add_insecure_port(args.listen) == 0:
        raise SystemExit(f"could not bind {args.listen}")
    server.start()
    print(f"mock QOP listening on {args.listen}", file=sys.stderr, flush=True)
    if args.ready_file:
        open(args.ready_file, "w").write("ready\n")
    try:
        server.wait_for_termination()
    except KeyboardInterrupt:
        server.stop(0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
