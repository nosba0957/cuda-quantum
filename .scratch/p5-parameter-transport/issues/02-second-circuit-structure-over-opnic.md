# 02: A second circuit structure over OPNIC behaves as the spike found

**What to build:** When a process sends a second **circuit structure** over the **OPNIC transport**, it does what ticket 01 proved the hardware can do. Today the executor silently reuses the link and hopes.

- If the spike says streams rebind: two structures work in one process, proven offline against the fake card.
- If not: selecting OPNIC for a second structure stops with a clear error naming the limit (one circuit structure per process over OPNIC), instead of hanging.

**Blocked by:** 01 (Live spike: one OPNIC stream setup serves two programs)

**Status:** resolved

- [x] An offline check covers the second-structure case on the fake card (works, or errors clearly)
- [x] The single-structure OPNIC run and the Ethernet and streamed transports still pass the P5 checks
- [x] The spec's risk note and TODO record the outcome

## Comments

**2026-10-18:** the spike said streams rebind, so the second structure simply
works; no error path is needed. `run_p5_checks.sh` gains an offline mode that
runs `tests/p5c_live.cpp --spike` over the fake card: both structures return
their shots, one Compile each, the first job cancelled before the second is
queued and the second at close. The other three modes still pass.
