#!/usr/bin/env python3
# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

# Proves qm_min.proto is wire-compatible with the real qm-qua messages: encode
# with the SDK's generated classes, decode with qm_min's descriptors, compare
# field values and re-serialized bytes. A wrong field number in qm_min.proto is
# silent corruption on the wire, so this is the only thing that catches it.
#
#   protoc --descriptor_set_out=qm_min.desc --include_imports qm_min.proto
#   ~/as_ntu_ncku/.venv/bin/python tests/check_qm_min_wire.py qm_min.desc

import sys

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory
from google.protobuf.wrappers_pb2 import Int64Value

from qm.grpc.qm.grpc.v2 import qmm_api_pb2, qm_api_pb2, job_api_pb2
from qm.grpc.qm.grpc.v2 import common_types_pb2
from qm.grpc.qm.pb import inc_qua_pb2, inc_qua_config_pb2, job_manager_pb2

failures = []


def load(desc_path):
    pool = descriptor_pool.DescriptorPool()
    fds = descriptor_pb2.FileDescriptorSet()
    with open(desc_path, "rb") as handle:
        fds.ParseFromString(handle.read())
    for f in fds.file:
        pool.Add(f)
    return pool, fds


def check(label, real, min_cls, expect):
    blob = real.SerializeToString()
    m = min_cls()
    consumed = m.MergeFromString(blob)
    got = {k: eval("m." + k, {"m": m}) for k in expect}
    ok = (consumed == len(blob) and m.SerializeToString() == blob and
          all(got[k] == v for k, v in expect.items()))
    if not ok:
        failures.append(label)
    print("%s %-34s %d/%d bytes %s" %
          ("OK  " if ok else "FAIL", label, consumed, len(blob), got))


def main(argv):
    if len(argv) != 2:
        print(__doc__ or "usage: check_qm_min_wire.py qm_min.desc")
        return 2
    pool, fds = load(argv[1])

    def Min(name):
        return message_factory.GetMessageClass(
            pool.FindMessageTypeByName("qm.grpc.v2." + name))

    prog = inc_qua_pb2.QuaProgram()
    prog.script.body.statements.add().play.qe.name = "q1_xy"
    check(
        "QmServiceCompileRequest",
        qm_api_pb2.QmServiceCompileRequest(quantum_machine_id="qm-7",
                                           high_level_program=prog),
        Min("QmServiceCompileRequest"), {
            "quantum_machine_id": "qm-7",
            "high_level_program": prog.SerializeToString()
        })

    # The blob our bytes field carries must reparse as a real QuaProgram.
    m = Min("QmServiceCompileRequest")()
    m.quantum_machine_id = "qm-7"
    m.high_level_program = prog.SerializeToString()
    back = qm_api_pb2.QmServiceCompileRequest()
    back.ParseFromString(m.SerializeToString())
    if back.high_level_program != prog:
        failures.append("QuaProgram reverse roundtrip")
    print("%s QuaProgram reverse roundtrip" %
          ("OK  " if back.high_level_program == prog else "FAIL"))

    cfg = inc_qua_config_pb2.QuaConfig()
    cfg.v1beta.controllers["con1"].type = "opx1000"
    check(
        "OpenQuantumMachineRequest",
        qmm_api_pb2.OpenQuantumMachineRequest(
            config=cfg,
            close_mode=qmm_api_pb2.OpenQuantumMachineRequest.CloseMode.
            CLOSE_MODE_IF_NEEDED), Min("OpenQuantumMachineRequest"), {
                "config": cfg.SerializeToString(),
                "close_mode": 1
            })

    check(
        "GetVersionResponse",
        qmm_api_pb2.GetVersionResponse(
            success=qmm_api_pb2.GetVersionResponse.GetVersionResponseSuccess(
                gateway="3.6.0", controllers={"con1": "1.2.3"})),
        Min("GetVersionResponse"), {
            "success.gateway": "3.6.0",
            "success.controllers['con1']": "1.2.3"
        })

    check(
        "CompileResponse",
        qm_api_pb2.CompileResponse(
            success=qm_api_pb2.CompileResponse.CompilationSuccess(
                program_id="pid-42")), Min("CompileResponse"),
        {"success.program_id": "pid-42"})

    check(
        "AddCompiledToQueueRequest",
        qm_api_pb2.QmServiceAddCompiledToQueueRequest(quantum_machine_id="qm-7",
                                                      program_id="pid-42"),
        Min("QmServiceAddCompiledToQueueRequest"), {
            "quantum_machine_id": "qm-7",
            "program_id": "pid-42"
        })

    check(
        "AddCompiledToQueueResponse",
        qm_api_pb2.AddCompiledToQueueResponse(
            success=qm_api_pb2.AddCompiledToQueueResponse.
            AddCompiledToQueueResponseSuccess(job_id="job-99")),
        Min("AddCompiledToQueueResponse"), {"success.job_id": "job-99"})

    check("QmServiceCloseRequest",
          qm_api_pb2.QmServiceCloseRequest(quantum_machine_id="qm-7"),
          Min("QmServiceCloseRequest"), {"quantum_machine_id": "qm-7"})

    check(
        "QmServiceCloseResponse",
        qm_api_pb2.QmServiceCloseResponse(
            success=qm_api_pb2.
            QmServiceCloseResponse.QmServiceCloseResponseSuccess()),
        Min("QmServiceCloseResponse"), {"HasField('success')": True})

    check(
        "PushToInputStream(fixed)",
        job_api_pb2.JobServicePushToInputStreamRequest(
            job_id="job-99",
            stream_name="input_stream_gamma",
            fixed_stream_data=job_manager_pb2.FixedStreamData(
                data=[0.5, -0.25])), Min("JobServicePushToInputStreamRequest"),
        {
            "job_id": "job-99",
            "stream_name": "input_stream_gamma",
            "fixed_stream_data.data[:]": [0.5, -0.25]
        })

    check(
        "GetJobStatus request",
        job_api_pb2.JobServiceGetJobStatusRequest(job_id="job-99"),
        Min("JobServiceGetJobStatusRequest"), {"job_id": "job-99"})

    resp = job_api_pb2.JobServiceGetJobStatusResponse()
    resp.success.status = common_types_pb2.RUNNING
    check("GetJobStatus response(RUNNING)", resp,
          Min("JobServiceGetJobStatusResponse"), {"success.status": 3})

    resp = job_api_pb2.JobServiceGetJobStatusResponse()
    resp.error.details = "no such job"
    check("GetJobStatus response(error)", resp,
          Min("JobServiceGetJobStatusResponse"),
          {"error.details": "no such job"})

    check(
        "PushToInputStream(int)",
        job_api_pb2.JobServicePushToInputStreamRequest(
            job_id="job-99",
            stream_name="input_stream_n",
            int_stream_data=job_manager_pb2.IntStreamData(data=[3, 4])),
        Min("JobServicePushToInputStreamRequest"),
        {"int_stream_data.data[:]": [3, 4]})

    req = job_api_pb2.GetNamedResultsRequest(
        job_id="job-99",
        outputs=[
            job_api_pb2.GetNamedResultsRequest.Output(
                output_name="c",
                range=common_types_pb2.Range(**{
                    "from": Int64Value(value=0),
                    "to": Int64Value(value=999)
                }))
        ])
    check(
        "GetNamedResultsRequest", req, Min("GetNamedResultsRequest"), {
            "job_id": "job-99",
            "outputs[0].output_name": "c",
            "outputs[0].range.to.value": 999
        })
    m = Min("GetNamedResultsRequest")()
    m.MergeFromString(req.SerializeToString())
    if getattr(m.outputs[0].range, "from").value != 0:
        failures.append("Range.from stand-in")

    check(
        "GetNamedResultResponse",
        job_api_pb2.GetNamedResultResponse(
            success=job_api_pb2.GetNamedResultResponse.
            GetNamedResultResponseSuccess(
                count_of_items=2, output_name="c", data=b"\x01\x00\x01\x01")),
        Min("GetNamedResultResponse"), {
            "success.count_of_items": 2,
            "success.output_name": "c",
            "success.data": b"\x01\x00\x01\x01"
        })

    print("\nmethod paths:")
    for f in fds.file:
        for s in f.service:
            for meth in s.method:
                print("  /%s.%s/%s%s" %
                      (f.package, s.name, meth.name,
                       "  [server-stream]" if meth.server_streaming else ""))

    print("\nfailures:", failures or "none")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
