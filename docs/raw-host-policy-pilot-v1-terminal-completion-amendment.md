# Raw-host policy pilot terminal-completion amendment

## Provenance

The `raw-host-policy-pilot-v1-study-v2` tag remains immutable at
`6416547dfa29aefc69d2fcf3bae34fcd3e639972`. The study-v2 collection followed
the frozen no-replacement rule and stopped at sequence 10. Lifecycles 1 through
9 passed; `policy-v2-study-b2r10` stopped with
`process emulation: generation did not become terminal`; lifecycles 11 and 12
were not dispatched. The receiver had completed all eight generations, but the
sender had emitted terminal records only for generations 0 through 6.

The incomplete collection is not used for policy comparison. Its frozen local
archive is
`raw-host-policy-pilot-v1-study-v2-invalid-attempt.tar.gz`, with SHA-256
`5c7b5c600f2537d371ed1c13a43b9e7ed2bec1a82521ff64656ebaf30be794d8`.
The independent post-teardown audit found no pilot instances, disks, addresses,
snapshots, firewall rules, subnetworks or networks.

## Root cause

Process protocol V2 repeated each terminal feedback datagram three times, then
the receiver exited as soon as every local generation was terminal. If all
copies needed by the sender were lost or rejected, the sender continued asking
for repair while no receiver remained to repeat the sticky terminal report.
The resulting nonzero sender exit made the lifecycle technically incomplete.

This is a transport-completion defect, not a delivery outcome and not evidence
for or against any policy. Reinterpreting receiver-only completion after the
fact would leave sender-clock RTT, repair and per-generation evidence missing,
so the nine completed lifecycles cannot be combined with a replacement run.

## Protocol V3 amendment

Protocol V3 adds a session-bound, direction-bound and replay-protected
`TERMINAL_ACK` frame:

1. The receiver emits a sticky terminal report and remains alive.
2. The sender accepts the first monotonic authenticated terminal report and
   applies it to the selected policy.
3. Only after policy observation, the sender emits three authenticated terminal
   ACK copies outside symbol impairment.
4. The receiver exits only after authenticating an ACK for every terminal
   generation. A subsequent descriptor or symbol for an already-terminal
   generation causes another terminal-feedback round.

ACK datagrams are counted separately from descriptors, initial symbols and
repair symbols. `receiver_service_elapsed_ms` still ends when all generations
become terminal; `terminal_ack_wait_ms` separately records the control-plane
completion interval. Evidence schema V4 also records the protocol version,
handshake identity, ACK count and terminal-feedback retry rounds.

No implementation or parameter of `fixed-minimum`, `fixed-class-aware` or
`biological-adaptive` changes in this amendment. The policy-pilot workload,
transport contracts, adverse traces, regime-change perturbation, randomized
order and analysis mode also remain unchanged.

## Regression and next gate

`process_terminal_recovery_v3.trace` deterministically drops the three final
terminal-feedback copies. The frozen V2 binary reproduces the study failure;
the V3 regression requires a later sticky terminal-feedback round, complete
sender evidence and authenticated ACKs for both generations.

No GCP dispatch is authorized by this amendment. After local, container/CI and
independent remote-host validation pass on one reviewed commit, a new source
tag and complete provenance freeze are required. The next GCP action must be a
new-identity technical preflight. Any subsequent 12-lifecycle collection must
start from sequence 1 and may not reuse study-v2 lifecycles.
