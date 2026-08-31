#!/usr/bin/env python3
# ============================================================================ #
# Copyright (c) 2022 - 2025 NVIDIA Corporation & Affiliates.                   #
# All rights reserved.                                                         #
#                                                                              #
# This source code and the accompanying materials are made available under     #
# the terms of the Apache License 2.0 which accompanies this distribution.     #
# ============================================================================ #

# The qm-qua SDK ships generated *_pb2.py but no .proto files. This rebuilds
# readable .proto text from the descriptors embedded in those modules, as a
# reference for hand-cutting qm_min.proto. Output is never compiled.
# Needs the venv python that has qm installed.

import argparse
import importlib
import os
import sys

from google.protobuf import descriptor_pb2

ROOT_MODULES = [
    "qm.grpc.qm.grpc.v2.qmm_api_pb2",
    "qm.grpc.qm.grpc.v2.qm_api_pb2",
    "qm.grpc.qm.grpc.v2.job_api_pb2",
    "qm.grpc.qm.grpc.v2.common_types_pb2",
]

SKIP_PREFIXES = ("google/protobuf/",)

FD = descriptor_pb2.FieldDescriptorProto

_SCALARS = {
    FD.TYPE_DOUBLE: "double",
    FD.TYPE_FLOAT: "float",
    FD.TYPE_INT64: "int64",
    FD.TYPE_UINT64: "uint64",
    FD.TYPE_INT32: "int32",
    FD.TYPE_FIXED64: "fixed64",
    FD.TYPE_FIXED32: "fixed32",
    FD.TYPE_BOOL: "bool",
    FD.TYPE_STRING: "string",
    FD.TYPE_BYTES: "bytes",
    FD.TYPE_UINT32: "uint32",
    FD.TYPE_SFIXED32: "sfixed32",
    FD.TYPE_SFIXED64: "sfixed64",
    FD.TYPE_SINT32: "sint32",
    FD.TYPE_SINT64: "sint64",
}


def _type_name(field):
    if field.type in (FD.TYPE_MESSAGE, FD.TYPE_ENUM, FD.TYPE_GROUP):
        return field.type_name.lstrip(".")
    return _SCALARS[field.type]


def _default_clause(field):
    if not field.HasField("default_value"):
        return ""
    val = field.default_value
    if field.type in (FD.TYPE_STRING, FD.TYPE_BYTES):
        val = '"%s"' % val
    return " [default = %s]" % val


def _map_entry(msg):
    if not msg.options.map_entry:
        return None
    key = next(f for f in msg.field if f.number == 1)
    val = next(f for f in msg.field if f.number == 2)
    return _type_name(key), _type_name(val)


def _render_field(field, indent, map_entries, syntax):
    pad = " " * indent
    label = ""
    if field.label == FD.LABEL_REPEATED:
        entry = map_entries.get(field.type_name.lstrip("."))
        if entry is not None:
            return "%smap<%s, %s> %s = %d;\n" % (pad, entry[0], entry[1],
                                                 field.name, field.number)
        label = "repeated "
    elif field.label == FD.LABEL_REQUIRED:
        label = "required "
    elif field.label == FD.LABEL_OPTIONAL:
        if syntax == "proto2" or field.proto3_optional:
            label = "optional "
    return "%s%s%s %s = %d%s;\n" % (pad, label, _type_name(field), field.name,
                                    field.number, _default_clause(field))


def _render_enum(enum, indent):
    pad = " " * indent
    out = "%senum %s {\n" % (pad, enum.name)
    if enum.options.allow_alias:
        out += "%s  option allow_alias = true;\n" % pad
    for value in enum.value:
        out += "%s  %s = %d;\n" % (pad, value.name, value.number)
    return out + "%s}\n" % pad


def _render_message(msg, indent, map_entries, syntax):
    if msg.options.map_entry:
        return ""

    pad = " " * indent
    out = "%smessage %s {\n" % (pad, msg.name)

    for nested in msg.nested_type:
        body = _render_message(nested, indent + 2, map_entries, syntax)
        if body:
            out += body + "\n"
    for enum in msg.enum_type:
        out += _render_enum(enum, indent + 2) + "\n"

    # proto3_optional fields carry a synthetic one-member oneof that must not
    # be printed as a real oneof.
    synthetic = {
        f.oneof_index for f in msg.field
        if f.proto3_optional and f.HasField("oneof_index")
    }
    plain = [
        f for f in msg.field
        if not f.HasField("oneof_index") or f.oneof_index in synthetic
    ]
    for field in plain:
        out += _render_field(field, indent + 2, map_entries, syntax)

    for idx, oneof in enumerate(msg.oneof_decl):
        if idx in synthetic:
            continue
        out += "%s  oneof %s {\n" % (pad, oneof.name)
        for field in msg.field:
            if field.HasField("oneof_index") and field.oneof_index == idx:
                out += _render_field(field, indent + 4, map_entries, syntax)
        out += "%s  }\n" % pad

    for rng in msg.reserved_range:
        end = rng.end - 1
        out += "%s  reserved %d%s;\n" % (pad, rng.start,
                                         "" if end == rng.start else " to %d" %
                                         end)
    for name in msg.reserved_name:
        out += '%s  reserved "%s";\n' % (pad, name)

    return out + "%s}\n" % pad


def _render_service(svc):
    out = "service %s {\n" % svc.name
    for m in svc.method:
        out += "  rpc %s (%s%s) returns (%s%s);\n" % (
            m.name, "stream " if m.client_streaming else "",
            m.input_type.lstrip("."), "stream " if m.server_streaming else "",
            m.output_type.lstrip("."))
    return out + "}\n"


def _collect_map_entries(fdp, into):

    def walk(msgs, scope):
        for msg in msgs:
            fqn = "%s.%s" % (scope, msg.name) if scope else msg.name
            entry = _map_entry(msg)
            if entry is not None:
                into[fqn] = entry
            walk(msg.nested_type, fqn)

    walk(fdp.message_type, fdp.package)


def render_file(fdp, map_entries):
    syntax = fdp.syntax or "proto2"
    out = "// Reconstructed from the descriptor pool of the installed qm-qua\n"
    out += "// package by tools/dump_protos.py.  Reference only -- not built.\n"
    out += "// Source file: %s\n\n" % fdp.name
    out += 'syntax = "%s";\n\n' % syntax
    if fdp.package:
        out += "package %s;\n\n" % fdp.package
    for dep in fdp.dependency:
        out += 'import "%s";\n' % dep
    if fdp.dependency:
        out += "\n"

    opts = fdp.options
    string_opts = ("java_package", "java_outer_classname", "go_package",
                   "csharp_namespace", "objc_class_prefix")
    for name in string_opts:
        if opts.HasField(name):
            out += 'option %s = "%s";\n' % (name, getattr(opts, name))
    if opts.HasField("java_multiple_files"):
        out += "option java_multiple_files = %s;\n" % str(
            opts.java_multiple_files).lower()
    if any(opts.HasField(n) for n in string_opts + ("java_multiple_files",)):
        out += "\n"

    for enum in fdp.enum_type:
        out += _render_enum(enum, 0) + "\n"
    for msg in fdp.message_type:
        body = _render_message(msg, 0, map_entries, syntax)
        if body:
            out += body + "\n"
    for svc in fdp.service:
        out += _render_service(svc) + "\n"
    return out


def load_descriptors(root_modules):
    files = {}
    seen = set()

    def visit(file_descriptor):
        if file_descriptor.name in seen:
            return
        seen.add(file_descriptor.name)
        for dep in file_descriptor.dependencies:
            visit(dep)
        if file_descriptor.name.startswith(SKIP_PREFIXES):
            return
        fdp = descriptor_pb2.FileDescriptorProto()
        fdp.ParseFromString(file_descriptor.serialized_pb)
        files[file_descriptor.name] = fdp

    for mod_name in root_modules:
        visit(importlib.import_module(mod_name).DESCRIPTOR)

    return files


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="dump qm-qua .proto text")
    ap.add_argument("-o", "--out", default=os.path.join(here, "proto_dump"))
    ap.add_argument("-m", "--module", action="append", default=None)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args(argv)

    try:
        files = load_descriptors(ROOT_MODULES + list(args.module or []))
    except ImportError as exc:
        sys.stderr.write("error: %s\nRun with the venv python that has qm.\n" %
                         exc)
        return 1

    map_entries = {}
    for fdp in files.values():
        _collect_map_entries(fdp, map_entries)

    for name, fdp in files.items():
        path = os.path.join(args.out, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as handle:
            handle.write(render_file(fdp, map_entries))
        print("wrote %s" % path)

    if args.check:
        print("\n--- services ---")
        for name, fdp in files.items():
            for svc in fdp.service:
                for m in svc.method:
                    kind = "bidi" if m.client_streaming and m.server_streaming \
                        else "client-stream" if m.client_streaming \
                        else "server-stream" if m.server_streaming else "unary"
                    print("/%s.%s/%-26s %-13s %s -> %s" %
                          (fdp.package, svc.name, m.name, kind,
                           m.input_type.lstrip("."), m.output_type.lstrip(".")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
