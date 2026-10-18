# 01: Live spike: one OPNIC stream setup serves two programs

**What to build:** On the real OPX, over the **OPNIC transport**, run two different **circuit structures** back to back in one process, with the parameter and result streams initialised once. Find out whether the second program's stream declarations rebind to the streams the process already holds. This is the spec's first risk ("OPNIC streams cannot be initialised twice in one process").

**Blocked by:** None (can start immediately)

**Status:** ready-for-human

Runs only in the user's reserved OPX time, with their explicit go-ahead: it plays pulses and uses `/dev/opnic0`. Check that nobody else's OPNIC job holds stream IDs 987/654.

- [ ] Two different circuit structures each complete at least one **evaluation** over OPNIC in one process, or the failure is captured (error text, which side hangs, job state)
- [ ] The waiting job is cancelled between programs and at close; the machine is released afterwards
- [ ] The answer (yes/no, with evidence) is recorded under `## Comments` here and in the spec's risk note
