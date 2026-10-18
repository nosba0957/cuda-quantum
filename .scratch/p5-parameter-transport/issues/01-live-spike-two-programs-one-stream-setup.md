# 01: Live spike: one OPNIC stream setup serves two programs

**What to build:** On the real OPX, over the **OPNIC transport**, run two different **circuit structures** back to back in one process, with the parameter and result streams initialised once. Find out whether the second program's stream declarations rebind to the streams the process already holds. This is the spec's first risk ("OPNIC streams cannot be initialised twice in one process").

**Blocked by:** None (can start immediately)

**Status:** ready-for-human

Runs only in the user's reserved OPX time, with their explicit go-ahead: it plays pulses and uses `/dev/opnic0`. Check that nobody else's OPNIC job holds stream IDs 987/654.

- [ ] Two different circuit structures each complete at least one **evaluation** over OPNIC in one process, or the failure is captured (error text, which side hangs, job state)
- [ ] The waiting job is cancelled between programs and at close; the machine is released afterwards
- [ ] The answer (yes/no, with evidence) is recorded under `## Comments` here and in the spec's risk note

## Comments

**2026-10-18, live on QPX1000_4 (commit 4c5880d5f4): yes, streams rebind.**
Two circuit structures (1 qubit, then 2 qubits), one 128-shot packet each,
both served by OPNIC streams initialised once in one process; the first job
was cancelled before the second was queued, and the machine was closed after
(the cluster lists no open machines). Two earlier attempts failed before any
pulse: the container lacked `/run/opnic`, then both streams shared one driver
handle, which pins once (EBUSY). Both fixed.

Open: every shot was identical (`0` x128 for `rx(1.0)`, `01` x128 for the
Bell-like circuit). The pattern is the same in both circuits (qubit 0 always
reads 0, qubit 1 always 1), which points at the chip's readout rather than the
packets; the same spike over Ethernet tells them apart.
