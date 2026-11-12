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
  On 2026-11-04 a physical Windows XP SP3 x86 machine (Lenovo E40) completed a 20-cycle
  plug/unplug run against the `cm` backend: 21 device sessions, 0 misses, worst block
  latency under 1 s, and 18 of 21 insertions arrived already disabled because the disabled
  state persists on the device instance. That run also exercised the
  request/approve/revoke chain end to end and found that a revoked or expired device stays
  readable for about 57 s while the devnode sits in `DN_WILL_BE_REMOVED` behind an open
  volume handle. That 57 s disconnect window, the three fixes built but not yet re-tested on
  hardware, notifier interaction, service-down behavior, TLS, upgrade, and removal remain
  outstanding.

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

## Passive installation backstop

`src/windows/policy_registry.cpp` publishes the documented Device Installation Restrictions
policy so that an unapproved device is still refused while the service is stopped. It is
compiled into the Windows 7 and Windows 10 kits and **excluded at compile time from
`legacy-xp-x86`**, because XP has no such policy layer; the XP binary contains none of that
code path and keeps exactly the behaviour it was qualified with.

- Windows 7 and 8.x have no layered evaluation, so default-deny is published as
  `DenyUnspecified=1` plus an `AllowDeviceIDs` list. `DenyRemovableDevices` is deliberately not
  set: without layering it outranks the allow list and would break every approval.
- Only instance IDs are published. Hardware and compatible IDs are model level and would approve
  every unit of a model the administrator never saw.
- A USB mass-storage device installs as a USB function node plus one or more USBSTOR children, so
  the approved device's devnode chain is expanded before publishing. A single instance ID is not
  enough for the device to install under `DenyUnspecified`.
- The before-image is written to `backstop-manifest.dat` in the ACL-protected data directory and
  values are restored only where the current value still equals what the agent wrote. Any
  externally owned value makes the agent report `external_policy_conflict` and leave the policy
  key untouched rather than overwrite a domain GPO or another product.
- The backstop survives a service stop by design and is therefore **not** reverted on exit. It is
  removed by `UsbDlp.LegacyAgent.exe --restore-backstop`, which `Uninstall-Client.cmd` runs before
  removing the MSI.
- Residual limitation, unchanged from the spec: a device that was already installed before the
  baseline was published can remain usable during a service outage, because
  `DenyUnspecified` only governs new installations.

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
