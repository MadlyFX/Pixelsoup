# Network recovery coordination

`UnresolvedCohortRecovery` is a caller-serialized, recovery-only state machine
for the first unresolved frame of one immutable WAL snapshot. It is deliberately
separate from `DirectorResumeSession`, `DirectorSessionService`, the live hub,
and the authoring scheduler, so the normal listener and resume path continue to
reject a WAL tail beyond the durable release frontier.

Creation accepts only a genesis-based snapshot/inventory pair whose cohort
journal coverage closes exactly at the captured WAL tail. It reconstructs an
unpublished checkpoint by replaying only the durably released prefix, then
exact-matches the selected journal plan's complete physical `WalFrameIdentity`
against the next typed snapshot batch. Non-genesis base checkpoints remain
unsupported until they can be authenticated rather than supplied by assertion.

An attempt must be durably recorded before admission. Each admitted session
must match one exact planned node ID and incarnation. The coordinator emits
value-owned checkpoint and prepare actions carrying the complete durable
attempt identity (token, Director epoch, and session namespace); the caller may
perform blocking socket work concurrently and submit the exact action result
afterward. Checkpoint activation must land exactly on the captured release
frontier, and only an exact `Prepared` acknowledgement counts. A `Committed`
acknowledgement is rejected. There is no commit-permit action or API.

Exact Prepared quorum calls the narrow recovery release authority once. A
durable result always ends in `RestartRequired`; the coordinator never changes
into normal Live operation. Version 1 handles a multi-frame unresolved suffix
with repeated clean recovery-process starts, releasing only its authenticated
front frame each time. A crash before release discards transient Prepared
evidence and records a higher durable attempt on reopen. A crash after release
is resolved from the release ledger on the next start, never from node reports
or journal inference.
