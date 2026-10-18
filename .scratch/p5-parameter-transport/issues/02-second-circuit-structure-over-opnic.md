# 02: A second circuit structure over OPNIC behaves as the spike found

**What to build:** When a process sends a second **circuit structure** over the **OPNIC transport**, it does what ticket 01 proved the hardware can do. Today the executor silently reuses the link and hopes.

- If the spike says streams rebind: two structures work in one process, proven offline against the fake card.
- If not: selecting OPNIC for a second structure stops with a clear error naming the limit (one circuit structure per process over OPNIC), instead of hanging.

**Blocked by:** 01 (Live spike: one OPNIC stream setup serves two programs)

**Status:** ready-for-agent

- [ ] An offline check covers the second-structure case on the fake card (works, or errors clearly)
- [ ] The single-structure OPNIC run and the Ethernet and streamed transports still pass the P5 checks
- [ ] The spec's risk note and TODO record the outcome
