# Raw-host policy pilot controller-resilience amendment

## Provenance and stopped collection

The immutable `raw-host-policy-pilot-v1-study-v3` tag points directly to
`c523e8020c7d96587ce37638d82d728a2513f04c`. Its separately identified
`policy-v3-preflight-01` passed authenticated protocol-V3 transport, byte
hashes, metadata, teardown and an independent zero-resource audit.

The subsequent study-v3 collection stopped without replacement at sequence 2.
Sequence 1 completed technically. During sequence 2, the Windows Cloud SDK
`plink`/IAP control connection for the receiver closed before the controller
observed `receiver_ready`. The sender was never dispatched and sequences 3
through 12 were not created. The partial collection is not analyzed for policy
performance.

The initial sequence-2 teardown also encountered transient nonzero control-plane
operations. A separate exact-name cleanup removed the two instances and disks,
four firewall rules, two subnetworks and two networks. The independent final
audit found no Aurora instances, disks, addresses, snapshots, firewall rules,
subnetworks or networks.

The stopped evidence archive is
`raw-host-policy-pilot-v1-study-v3-invalid-attempt.tar.gz`, with SHA-256
`db55b8d95069b448373545f712579be4a4150afaec6cfef26da0718d221b45be`.

## Controller-only correction

The receiver process is now independent of the lifetime of any one SSH/IAP
control connection:

1. A short, idempotent SSH command launches the receiver under `nohup` and
   records its PID, stdout, stderr and exit status in fixed files on the VM.
2. Ambiguous launch failures are retried with bounded delays. A live PID or a
   completed status file prevents a duplicate receiver launch.
3. Readiness and completion are observed through fresh bounded SSH probes.
   Transient probe failures do not kill the remote receiver and are counted in
   controller evidence.
4. The sender is dispatched only after an authenticated `receiver_ready` record
   has been observed. A remote nonzero receiver status before readiness remains
   a terminal technical failure.
5. Receiver stdout and stderr are copied as exact files after remote completion,
   preserving byte-level evidence hashes. The receiver process exit status is
   recorded separately from the transient SSH control sessions.

Cleanup remains exact-name and never uses wildcards or project deletion. It now
performs up to three bounded delete-and-audit passes with backoff, retries only
resources still reported by the previous audit, and can explicitly remove a
residual boot disk after its instance is gone. Every pass records delete return
codes and the complete seven-class remaining-resource audit. Success still
requires an entirely empty audit.

## Scientific boundary and next gate

This amendment does not change the implementations or parameters of
`fixed-minimum`, `fixed-class-aware` or `biological-adaptive`. It does not change
the process binary, protocol V3, workload, traces, regime-change perturbation,
randomization seed/order, topology, measurement schema or descriptive analysis
rules. The frozen experimental source remains `c523e802...`; this change affects
only the external GCP controller.

No GCP dispatch is authorized by this amendment. After local and CI validation,
the controller change requires review and merge. A future attempt requires a
new study identity and a new single technical preflight. Only an entirely
passing preflight with final audit zero can authorize a fresh sequence 1 through
12 collection; no lifecycle from study-v3 may be reused.
