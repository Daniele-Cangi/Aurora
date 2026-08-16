# Raw-host policy pilot v1 execution amendment

## Status

The first collection identity, `raw-host-policy-pilot-v1-study-v1`, is closed as
technically invalid. It produced zero valid VM-pair lifecycles and contributes
no policy outcome. Its frozen failure evidence is published in the corresponding
[GitHub prerelease](https://github.com/Daniele-Cangi/Aurora/releases/tag/raw-host-policy-pilot-v1-study-v1).

The failure occurred before transport execution in lifecycle 1. Teardown passed,
and the final resource audit reported no remaining study instances, disks,
addresses, snapshots, firewall rules, subnets, or networks.

## Root cause

The frozen controller generated an ephemeral OpenSSH private/public key pair
with `ssh-keygen`. Google Cloud CLI selects its bundled PuTTY suite on Windows,
where `gcloud compute ssh` and `gcloud compute scp` instead require the same key
material to include a companion `.ppk` file. With `--quiet`, gcloud correctly
refused the interactive overwrite needed to regenerate the incomplete key set.

## Controller-only correction

The controller now selects the key generator that matches gcloud's platform SSH
suite:

- Windows uses the Cloud SDK's bundled `winkeygen.exe` non-interactively and
  verifies the private, `.pub`, and `.ppk` files before provisioning continues.
- Other platforms retain the existing OpenSSH `ssh-keygen` path and verify the
  private/public pair.

This correction does not change the authenticated transport binary, policy
implementations or parameters, workload, traces, conditions, randomization seed
or order, topology, or measurement schema. The experimental source remains the
frozen commit `6416547dfa29aefc69d2fcf3bae34fcd3e639972` and must continue to be
passed explicitly as `--source-commit`.

## Gate for any new collection

`study-v1` must not be resumed and its identity must not be reused. Any future
collection requires a new immutable study identity and a separately identified,
non-experimental SSH/build/cleanup preflight. The preflight must pass, including
an empty resource audit, before the frozen 12-lifecycle order is dispatched.
There is no replacement lifecycle after a technical failure. No GCP dispatch is
authorized by this amendment itself.
