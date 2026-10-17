# One parameter table, a switchable transport, and OPNIC through libopnic

Variational parameters reach the OPX by a parameter transport chosen per run
(Ethernet by default, OPNIC by flag), and every transport shares one compiler
pass that leaves the parameters open and records a parameter table. The
transports differ only in the QUA receive instruction and the link that carries
the numbers, so a pass per transport would duplicate the hard part. The QUA is
still written by `qua_build.py`; a QUA dialect in MLIR is deferred to P6.

OPNIC is driven by `QMExecutor` calling QM's `libopnic` in-process, not through
CUDA-Q Realtime, even though QM has integrated OPNIC with Realtime. Realtime is
built for the controller to call the GPU mid-program (QEC decoding,
calibration), while a QAOA loop is the host pushing parameters and reading
counts; using it would mean inverting COBYLA into a handler and emitting
Realtime's RPC frames from QUA. No OPNIC bridge for Realtime is installed on
`gh1`, only QM's own `opnic` SDK. And an evaluation takes ~84 ms, mostly shots,
so Realtime's microsecond round trip buys nothing measurable here.

## Consequences

The OPNIC calls sit behind a narrow link interface, so a Realtime-based link
can replace `libopnic` later without touching the parameter table or
`max_cut`. Revisit this when a workload needs the OPX to call the GPU inside
one shot.
