# Native legacy compatibility implementation

This directory contains the C++14 implementation used for Windows 7 x86/x64 and the
source profile for Windows XP SP3 x86. It includes the service agent, on-demand native
notifier, compact server, strict JSON and policy evaluator, DPAPI persistence, MSI
projects, and deployment-kit templates.

## Current qualification status

- `legacy-win7-x86`: cross-built with MSVC v141; policy and DPAPI tests pass on the
  Windows 11 build host; Windows 7 VM runtime qualification is still required.
- `legacy-win7-x64`: cross-built with MSVC v141; policy and DPAPI tests pass on the
  Windows 11 build host; Windows 7 VM runtime qualification is still required.
- `legacy-win10-x86` and `legacy-win10-x64`: distinct Windows 10 native profiles; policy and
  DPAPI tests pass on the Windows 11 build host; Windows 10 VM qualification is still required.
- `legacy-xp-x86`: cross-built with MSVC `v141_xp` and the Windows 7.1A SDK; binaries use
  the Windows 5.01 subsystem and static C++ runtime; policy and DPAPI tests pass on the
  Windows 11 build host. A Windows XP Professional SP3 Hyper-V smoke run on 2026-08-28
  passed agent/server MSI installation, live default-deny service startup, enrollment,
  policy synchronization, native policy/DPAPI tests, and authenticated remote inventory.
  Physical USB enforcement, notifier interaction, service-down behavior, TLS, upgrade,
  removal, and real-hardware qualification are still required.

The native HTTP transport intentionally refuses to imply production TLS support. The
client requires `AllowInsecureHttp=1`, and the server requires the equivalent explicit
setting. These builds are suitable only for isolated compatibility testing until the
portable pinned-TLS dependency and its conformance tests are added.

## Build

Configure x86 and x64 with `USB_DLP_TARGET_PROFILE=legacy-win7-x86` or
`legacy-win7-x64`; use `legacy-win10-x86` or `legacy-win10-x64` for Windows 10.
Configure XP x86 with the `v141_xp` toolset and
`USB_DLP_TARGET_PROFILE=legacy-xp-x86`. Build `Release`, then run
`scripts/build-legacy-kits.ps1`. Generated kits are placed under
`artifacts/legacy-kits` and are marked `UNVERIFIED`.

## XP Hyper-V smoke test

The repeatable XP lab helpers are `scripts/setup-xp-hyperv-vm.ps1`,
`scripts/configure-xp-hyperv-network.ps1`, `scripts/stage-xp-hyperv-tests.ps1`,
`scripts/start-xp-hyperv-vm.ps1`, and `scripts/collect-xp-hyperv-results.ps1`.
They require an elevated PowerShell process and constrain disk operations to the named
XP lab VM. The guest uses an isolated internal switch, host `192.168.77.1`, guest
`192.168.77.10`, and an XP-compatible emulated Intel 21140 network adapter.

Hyper-V Generation 1 does not provide direct physical USB-device passthrough for this
test setup. The smoke run therefore qualifies installation, service, persistence, policy,
and API behavior only; Gate F hardware-enforcement cases must run on physical XP hardware
or an independently qualified USB-over-IP lab.

## Server administration

The server supports local administrator commands `--requests`, `--workstations`,
`--approve`, `--deny`, and `--revoke`. The same request and inventory operations are
available under `/api/v1/admin` with HTTP Basic authentication using the protected
`AdminPasswordFile`. Credentials and full server state are hashed or DPAPI protected.
