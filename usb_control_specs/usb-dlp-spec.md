# USB Device Control - Technical Specification

Version 1.4 - 2026-08-27 - multi-generation Windows - approximately 300 managed workstations

This document is normative. `MUST`, `MUST NOT`, `SHOULD`, and `MAY` have their RFC 2119
meanings. The test plan is the acceptance authority. Screenshots under `screenshots/en` and
`screenshots/zh` are the visual authority where they do not conflict with this document.

## 1. Purpose, scope, and threat model

The product prevents careless leakage of design and manufacturing IP through removable USB
storage and portable-device file transfer, while producing attributable audit records.

The v1 product:

- governs USB mass-storage devices and WPD/MTP/PTP file-transfer devices;
- supports allow and block policy at global, group, and workstation scope;
- continues enforcing the last valid policy while offline;
- notifies interactive users and supports time-bounded access requests;
- provides a bilingual web administration console and Windows notifier;
- maintains searchable USB-device and workstation inventory with pair-specific effective access; and
- is deployed and operated as an enterprise Windows service.

Explicit non-goals:

- resistance to a user with local administrator or SYSTEM privileges;
- BadUSB, HID spoofing, DMA, network, Bluetooth, optical-media, or cloud DLP protection;
- file content inspection, classification, encryption, or copy accounting;
- kernel-mode interception in the baseline v1 product (the separate XP filter qualification
  track in section 6.1 is experimental, not a release-qualified exception); and
- macOS or Linux support in v1.

Windows XP and Windows 7 are compatibility targets because of legacy manufacturing equipment,
not trusted security foundations. Their end of vendor servicing means the product cannot make the
host OS secure, prevent a local administrator from bypassing controls, or provide the same
exploit-resistance as a supported Windows release. Every legacy deployment MUST be isolated,
explicitly risk-accepted, and visibly identified in the administration console and audit exports.

The system provides attribution and deterrence, not absolute prevention. A user-mode race exists
between PnP arrival and devnode disable in the user-mode tracks. The target is enforcement within
1 second; copying during the race is an accepted residual risk and MUST be stated in deployment
documentation. The experimental XP filter track has a different kernel I/O boundary that requires
its own boot and physical-I/O qualification.

## 2. Supported environment

The release-qualified matrix is:

| OS | Architecture | Agent/notifier | Server | Profile |
|---|---|---|---|---|
| Windows XP Professional SP3 | x86 | required | required with legacy-risk mode | `legacy-xp-x86` |
| Windows 7 SP1 | x86, x64 | required | required with legacy-risk mode | `legacy-win7-x86`, `legacy-win7-x64` |
| Windows 10 Enterprise 22H2 | x86, x64 | required | required | `modern-win10-x86`, `modern-win10-x64` |
| Windows 11 Enterprise 23H2 or newer | x64 | required | test/development only | `modern-win11-x64` |
| Windows Server 2022/2025 | x64 | not an endpoint target | preferred production server | `modern-server-x64` |

XP means 32-bit Windows XP Professional SP3 only. XP x64, pre-SP3 XP, Windows 7 without SP1,
Windows Home, Windows on ARM, VDI device redirection, and multi-user RDS endpoints are excluded.
The installer MUST reject an OS/architecture outside its exact profile. Windows 10 is supported
only while covered by Microsoft servicing or an applicable ESU contract. XP and Windows 7 require
a customer legacy-support agreement, an isolated management network, documented compensating
controls, and quarterly risk review; they are never described as vendor-supported operating
systems.

Modern servers use .NET 8 and support SQLite for one instance and up to 500 workstations;
PostgreSQL 16 or newer remains required for larger or multi-instance deployments. Legacy servers
use the native compatibility implementation and SQLite only, are limited to one instance and 100
workstations, and MUST NOT join a cluster. A modern server remains the preferred design even when
legacy agents are present.

## 3. Architecture and solution layout

```text
Modern workstation
  UsbDlp.Agent       Windows Service, .NET 8, LocalSystem, x86/x64
    DeviceMonitor -> PolicyEvaluator -> Enforcer
    SyncClient | PolicyCache | EventQueue | NotifierPipeServer
  UsbDlp.Notifier    on-demand per-session WPF companion, .NET 8
             | HTTPS/TLS, certificate pins, per-agent credential
             v
Modern server
  UsbDlp.Server      ASP.NET Core 8
    Agent REST API | Razor Pages admin UI | PolicyCompiler | EF Core
```

Legacy XP/Windows 7 uses a parallel native Win32 implementation because no supported .NET runtime
spans the full matrix:

```text
Legacy workstation
  UsbDlp.LegacyAgent       native Win32 service, LocalSystem
  UsbDlp.LegacyNotifier    on-demand native Win32 per-session UI
       SetupAPI/CfgMgr32 adapter | policy cache | event queue | portable TLS
  RuisiUsbDlpFilter.sys    optional XP-only experimental USBSTOR disk upper filter
                              | canonical /api/v1 JSON
Legacy server
  UsbDlp.LegacyServer      native Win32 service
       HTTP/API host | portable TLS | SQLite | embedded static admin assets
```

The legacy implementation MUST NOT load the CLR. It uses only APIs available on its declared
minimum OS, a statically linked runtime, UTF-8 internally, and explicit x86/x64 builds. The modern
and legacy implementations share generated JSON schemas, canonical policy test vectors, database
migration fixtures, localization source, and API conformance tests. They do not share binary
assemblies. Any different decision, normalized identity, authorization result, or wire response
for the same conformance vector is a release blocker.

The full administration web UI may be served by a legacy server but MUST be used from a separately
maintained modern browser. Installing an obsolete browser engine on XP/Windows 7 is prohibited.
The legacy server provides a local command-line recovery tool for health, backup, certificate,
password and token operations when no browser is reachable.

One `UsbDlp.sln` MUST contain:

- `UsbDlp.Shared` (`net8.0`): DTOs, serialization contracts, policy model and evaluator;
- `UsbDlp.Windows` (`net8.0-windows`): Windows interop and pure device-identity helpers;
- `UsbDlp.Agent` (`net8.0-windows`): service host, synchronization and enforcement;
- `UsbDlp.Notifier` (`net8.0-windows`): WPF per-user notifier;
- `UsbDlp.Server` (`net8.0`): API, compiler, persistence and Razor Pages UI;
- `UsbDlp.Cli` (`net8.0-windows`): diagnostics and dry-run harnesses; and
- `UsbDlp.Tests` plus `UsbDlp.IntegrationTests` (`net8.0`).

Legacy source is isolated under `legacy/` and produces `UsbDlp.LegacyAgent`,
`UsbDlp.LegacyNotifier`, `UsbDlp.LegacyServer`, `UsbDlp.LegacyAdmin`, and native contract tests.
No legacy compatibility change may lower warnings, crypto, authentication, or validation in the
modern projects.

Production projects MUST enable nullable reference types and treat compiler warnings as errors.

## 4. Device scope and canonical identity

### 4.1 In-scope devices

A present devnode is in scope only when all applicable conditions are true:

1. Its PnP ancestry resolves to the USB enumerator.
2. It represents one of:
   - a disk whose service is `USBSTOR`;
   - a storage volume whose parent chain resolves to such a disk; or
   - a WPD devnode in setup class `{eec5ad98-8080-425f-922a-dabf3de3f69a}` with MTP/PTP
     file-transfer capability.
3. It is not covered by the hard-coded safety allowlist.

When a physical USB peripheral exposes both a `USBSTOR` disk and one or more WPD volume children,
the monitor MUST select the `USBSTOR` disk as the governed devnode. Enumeration order MUST NOT
affect this choice. Disabling a storage device MUST target that disk node so all partitions and
drive letters on multi-volume media (for example a data partition plus an EFI partition) are
blocked together. A WPD child is selected only when no storage disk exists for that canonical
physical identity.

The generic Disk setup class `{4d36e967-e325-11ce-bfc1-08002be10318}` MUST NOT by itself make a
device in scope and MUST NEVER be written as a denied setup class. This prevents internal disks
from being affected.

Identity enrichment is not an admission condition. Once a devnode is safely identified as an
in-scope `USBSTOR` disk or WPD transfer device, a temporarily unavailable VID, PID, serial number,
friendly name, or USB-parent property MUST NOT cause the monitor to discard it. The agent evaluates
the available identity fields, applies the workstation default when no entry matches, and records
missing fields as `unknown` in diagnostics. In particular, default-deny remains fail-closed during
the incomplete Plug and Play metadata window observed on Windows XP.

### 4.2 Hard-coded safety allowlist

The agent MUST never disable a devnode in, or an ancestor required by, these categories: keyboard,
mouse, HID, smart-card reader, printer, USB host controller, USB hub, audio, Bluetooth, network,
system, and camera/webcam. The implementation MUST use both setup-class checks and ancestry checks;
policy cannot override this list. `Image` and `Camera` setup classes are always allowed in v1.

A safety-class ancestor of a governed storage disk does not exempt the disk: every USB storage
device is downstream of a hub/controller. Ancestry is used to identify the bus and to ensure
the selected enforcement target is the disk rather than a shared parent. The agent MUST NOT
select a `USB\\VID_*` parent merely because its service is `USBSTOR`.

The exact GUID table MUST live in a named, unit-tested `HardCodedSafetyAllowList`; each GUID MUST
have a source comment and friendly category name. Unknown classes are not safety-allowed merely
because they are unknown; they are governed only if they satisfy section 4.1.

### 4.3 Canonical `DeviceInfo`

The monitor MUST resolve one canonical physical identity for all child devnodes belonging to the
same USB device:

```json
{
  "instanceId": "USB\\VID_0781&PID_5583\\4C530001230904",
  "hardwareIds": ["USB\\VID_0781&PID_5583&REV_0100", "USB\\VID_0781&PID_5583"],
  "deviceInstallIds": ["USB\\VID_0781&PID_5583\\4C530001230904", "USB\\VID_0781&PID_5583"],
  "vid": "0781",
  "pid": "5583",
  "serial": "4C530001230904",
  "classGuid": "{eec5ad98-8080-425f-922a-dabf3de3f69a}",
  "service": "WpdFs",
  "friendlyName": "Example device",
  "transport": "usb",
  "capabilities": ["mtp"]
}
```

`instanceId` is the highest USB ancestor that provides VID/PID and uniquely represents the
physical peripheral. For WPD children, the monitor MUST walk parents with Configuration Manager
until it finds that ancestor. All instance IDs, hardware IDs, VID, PID and serial comparisons are
ordinal case-insensitive. VID/PID are normalized to four uppercase hexadecimal characters.

`deviceInstallIds` is the deduplicated set of PnP instance, hardware, and compatible IDs required
to install the governed devnode through its removable-device ancestry. It excludes hubs,
controllers, and all safety-allowed ancestors. These IDs support the passive installation policy
and are not used by the active evaluator. For USB storage, the exact `USBSTOR` disk instance ID
MUST be present and MUST be the active-enforcement target; a hardware ID that merely begins with
`USBSTOR` is not an interchangeable devnode instance ID.

If the USB instance token starts with `&`, contains location-derived Windows tokens, is absent, or
the bus does not report it as unique, `serial` MUST be null. The product MUST NOT present a
location-derived token as a hardware serial number.

### 4.4 Monitoring

The service MUST register `CM_Register_Notification` with
`CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE` and `CM_NOTIFY_FILTER_FLAG_ALL_DEVICE_INSTANCES`. The
callback MUST enqueue work and return immediately. Arrival/started/removed events trigger a
debounced (250 ms) re-enumeration; callbacks MUST NOT perform blocking I/O.

After registration, at service start, resume from sleep, and policy change, the monitor MUST call
`SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT)`, enumerate devnodes, and
filter them using section 4.1. Registration MUST occur before initial enumeration to avoid gaps.
Duplicate child notifications MUST collapse to one canonical device event.

Notifications are wake-up hints, not the only enforcement mechanism. The agent MUST also perform
a full present-device reconciliation on its configured local poll (2 seconds in the development
profile, configurable for production). This reconciliation closes missed-event and rapid
unplug/replug gaps.

If Configuration Manager notification registration fails, WMI `Win32_DeviceChangeEvent` MAY be
used only as a wake-up signal for full enumeration. Failure of both mechanisms is an
`enforce_error`, and the service MUST retry registration with capped exponential backoff.

## 5. Policy model and evaluation

### 5.1 Types

```text
MatchType  = instance_id | vid_pid_serial | vid_pid | device_class
Action     = allow | block
GroupMode  = default_deny | default_allow
ScopeType  = global | group | workstation
Decision   = allow | block
```

A `device_class` entry uses the product classes `usb_storage` or `wpd`, not a Windows
setup-class GUID or service name. Hardware-level identifiers are implementation details of
section 4.

### 5.2 Compiled policy wire contract

Policy JSON uses UTF-8, camelCase property names, lowercase enum strings, ISO 8601 UTC timestamps
with `Z`, and UUID strings. Unknown properties MUST be ignored for forward compatibility; unknown
enum values or a newer unsupported `schemaVersion` MUST reject the document.

```json
{
  "schemaVersion": 1,
  "policyVersion": 42,
  "generatedAtUtc": "2026-08-18T12:00:00Z",
  "workstationId": "00000000-0000-0000-0000-000000000001",
  "groupId": "00000000-0000-0000-0000-000000000002",
  "groupMode": "default_deny",
  "entries": [{
    "id": "00000000-0000-0000-0000-000000000003",
    "matchType": "vid_pid_serial",
    "vid": "0781",
    "pid": "5583",
    "serial": "4C530001230904",
    "instanceId": null,
    "deviceClass": null,
    "displayName": "SanDisk Ultra",
    "action": "allow",
    "mandatory": false,
    "scopeType": "workstation",
    "scopeId": "00000000-0000-0000-0000-000000000001",
    "expiresAtUtc": "2026-09-17T12:00:00Z",
    "note": "Approved request",
    "deviceInstallIds": ["USB\\VID_0781&PID_5583\\4C530001230904"]
  }]
}
```

Required fields depend on match type: `instanceId`; `vid`, `pid`, and non-null `serial`;
`vid` and `pid`; or `deviceClass`, respectively. Other match fields MUST be null. String limits
are: instance ID 200, serial 128, display name 200, note 2000. Entries with invalid combinations
MUST be rejected by the server and compiler.

`deviceInstallIds` contains 0-64 unique strings of 1-200 characters. Access-request approvals
MUST copy the agent-observed IDs. A manually created allow without installation IDs remains valid
for active enforcement, but the UI MUST warn that it cannot provide a passive reinstall exception
until the device has been observed and its identifiers attached.

`mandatory` is required and defaults to false only when reading legacy schema-version-1 data.
It may be true only when `scopeType=global` and `action=block`. The server MUST reject all other
combinations. A mandatory block is a security baseline that group and workstation entries cannot
override.

### 5.3 Evaluation

The evaluator receives an explicit UTC clock value. It MUST:

1. return `allow` for the hard-coded safety allowlist;
2. ignore entries where `expiresAtUtc <= nowUtc`;
3. evaluate all matching mandatory global blocks; if any match, return `block`, selecting the
   most-specific match and then the lowest entry UUID as a deterministic tie-break;
4. inspect ordinary entries by scope in this order: `workstation > group > global`; select the
   first scope containing at least one active matching entry and ignore lower scopes;
5. within that selected scope, choose matching specificity:
   `instance_id > vid_pid_serial > vid_pid > device_class`;
6. within the same scope and specificity, let `block` beat `allow`; if otherwise tied, select
   the lowest entry UUID; and
7. use the workstation group mode when no entry matches.

The returned result MUST include decision, matched entry ID or null, reason code, policy version,
and evaluation timestamp. Policy order in JSON MUST have no effect.

## 6. Enforcement and ownership

### 6.1 Active enforcement

For `block`, locate the governed target devnode and call `CM_Disable_DevNode` with
`CM_DISABLE_UI_NOT_OK`. V1 intentionally does not use `CM_DISABLE_PERSIST`: physical removal and
reinsertion resets the OS disable, after which the current policy is evaluated again. If a parent
must be disabled, the agent MUST first prove it is in scope and not safety-allowed. Each OS result
code is logged, including the Configuration Manager result code on failure.

For isolated XP SP3 x86 qualification, two **separate, non-production** enforcement tracks
replace the USBSTOR part of that call path. They MUST NOT be installed together. The SetupAPI
track applies `DIF_PROPERTYCHANGE` with `DICS_DISABLE` or `DICS_ENABLE` and
`DICS_FLAG_GLOBAL` to the exact USBSTOR disk instance ID. It logs the class-installer result,
reboot requirement, `CM_Get_DevNode_Status` flags and problem code. A successful installer call
is not proof of denial: only `DN_HAS_PROBLEM` with `CM_PROB_DISABLED` may be reported as
`blocked_verified`. Agent-owned disabled IDs and their policy identities persist in the
ACL-protected state store so a restart can re-enable a device after approval. A replugged ID is
re-evaluated on every local poll. Windows may require a replug for volume remounting.

The XP filter track registers a demand-start disk-class upper filter, but its `AddDevice` MUST
attach only when the PDO enumerator is `USBSTOR`, leaving internal disks and other device classes
untouched. A newly attached USBSTOR disk is denied by default in the driver before the agent
grants it. The LocalSystem agent resolves the exact disk PDO name from the canonical instance ID
and uses a SYSTEM/Administrators-only control device to set or query its grant. The driver denies
disk I/O while ungranted and removes the grant with the attachment; an `allow` policy evaluation
explicitly grants that device, including if it was already denied before agent startup. Revocation
removes the grant. No grant is inferred from a missing driver or an unmatched PDO. The notifier
and audit log MUST say `enforce_unverified` rather than claiming a block in those cases. This
track does not replace the WPD enforcement path and requires XP hardware validation of every
volume and I/O path before any security claim. The installation helper preserves other disk-class
`UpperFilters` entries, and rollback unregisters only its own filter and requires a reboot.

XP qualification finding (2026-10-19): the 1.1.6 SetupAPI test left a usable access interval
after manual re-enable in Device Manager; a later `blocked_verified` only proves the state at
that instant and cannot undo reads already made. The 1.1.7 filter log showed no verified block:
every filter request failed with result 2. Result 2 is ambiguous without the operation stage
(Win32 `ERROR_FILE_NOT_FOUND` versus Configuration Manager `CR_OUT_OF_MEMORY`). Neither build
is release-qualified. The diagnostic successor logs the failure stage (`locate_devnode`,
`read_pdo_name`, `open_filter_control`, `filter_ioctl`, or `device_not_attached`) and supplies a
post-reboot check of the driver file, service, disk-class registration, control channel, and
attached USB disk count. A healthy control channel is only a prerequisite for physical I/O tests,
not proof of blocking. Until those tests pass on XP, this track remains experimental.

The agent records ownership only after a successful disable. It may call `CM_Enable_DevNode` only
for that exact agent-owned instance ID and MUST never enable a device disabled by an administrator,
another product, policy, or Windows. A successful enable clears ownership. Physical removal clears
service-lifetime ownership in the default Config Manager track because a later instance is a new
enforcement cycle. The SetupAPI track instead persists only agent-owned disabled IDs until verified
re-enable; the filter track stores no OS devnode-disable ownership for USBSTOR because its grant
state is held per attachment in the driver.

XP test-track rollback MUST stop and wait for the agent service before changing ownership state.
It restores only recorded agent-owned disabled IDs and refuses agent removal if any such device
cannot be verified or re-enabled; the tester reconnects those devices and retries. Filter-track
rollback removes only its own disk-class `UpperFilters` entry and service, then requires a reboot.

Enforcement is serialized per canonical instance ID. An unchanged `allow` result may be cached.
An unchanged `block` result MUST NOT be trusted from cache alone: on every local reconciliation the
agent queries `CM_Get_DevNode_Status`. It emits no duplicate event while the node still reports
`CM_PROB_DISABLED`; if the same instance ID is enabled again, including after a rapid replug whose
removal notification was missed, it disables the node again and emits a new blocked event. A
policy-version change re-evaluates all devices regardless of cached decisions.

On transition to `allow`, the agent enables an agent-owned node and re-enumerates its parent so
volumes become usable without another workstation action. If Windows cannot safely enable it
because ownership was lost across agent restart, the notifier MUST explain that one unplug/replug
is required. No reboot, Device Manager action, service restart, or administrator action on the
workstation may be required.

### 6.2 Passive installation backstop and service-down behavior

The backstop uses documented Device Installation Restriction policy only. It MUST NOT write
`Deny_Read`, `Deny_Write`, or `Deny_All` under `RemovableStorageDevices`, because those
class-wide access controls cannot preserve per-device approvals. It MUST NOT deny the generic Disk,
USB controller, USB hub, HID, Keyboard, or Mouse setup classes.

For a default-deny workstation, the planner enables the Windows policies “Prevent installation of
removable devices” and “Apply layered order of evaluation.” It writes agent-owned allow instance,
hardware, and compatible IDs from active allow entries' `deviceInstallIds`. Windows layered
precedence therefore permits approved device IDs over the broader removable-device denial. For a
default-allow workstation, the broad denial is disabled and explicit block installation IDs are
written to the corresponding prevent-instance/prevent-device ID lists. Expired entries are omitted.

The backstop is a machine-wide security baseline and may prevent installation of non-USB devices
that Windows marks removable while a workstation is default-deny. This broader service-down effect
is accepted for v1 and MUST be called out in deployment documentation and the policy UI.

An allow entry with no `deviceInstallIds` MUST NOT weaken the broad baseline. While the service is
healthy, active enforcement still honors it for an already installed device. The UI and event
details MUST expose `passiveExceptionAvailable=false`; administrators can populate the IDs by
connecting the device under supervised dry-run or approving an observed request.

The agent MUST snapshot existing managed values before first write and MUST NOT overwrite or delete
values owned by domain GPO, MDM, or another product.

If conflicting external policy is detected, the agent enters `external_policy_conflict`, emits an
event, leaves external values untouched, and continues active enforcement. Deployments SHOULD use
an installation prerequisite that reserves the relevant policy values for UsbDlp or configure the
equivalent baseline centrally through GPO/MDM.

Agent-owned list values use a reserved deterministic name prefix where the Windows policy format
permits it; otherwise the agent stores an exact before/after manifest in its ACL-protected state
database and removes only values whose current content still equals what it wrote. Registry changes
are transactional as far as practical: stage, validate, then publish. A failed update restores the
previous agent-owned set.

If the agent stops or crashes, a devnode already disabled in the current attachment normally
remains disabled until physical removal, while new unapproved removable devices are denied
installation by the layered baseline. SCM recovery restarts the agent as specified in section 14.
Residual limitation: a previously installed device that the active agent has never evaluated may
remain usable during the service outage. This accepted user-mode limitation MUST appear in the
threat model, deployment guide, and release notes.

Windows 7 has no layered evaluation: the "Apply layered order of evaluation for Allow and
Prevent device installation policies across all device match criteria" policy requires Windows
10 2106 or Windows Server 2016, so it cannot be claimed on the 6.1 line. The OS adapter MUST
express default-deny as `DenyUnspecified=1` plus an `AllowDeviceIDs` list, the Vista-era form of
the same policy, and MUST NOT combine `DenyRemovableDevices=1` with that allow list: without
layering every specific deny outranks every allow, so the allow list would be neutralised and
per-device approval would stop working. The allow list MUST carry instance IDs only. Hardware
and compatible IDs are model level and would approve every unit of that model. Because one USB
mass-storage device installs as several devnodes, the adapter MUST expand the approved device's
devnode chain (the USB function node and its USBSTOR children) before publishing, or the approved
device still fails to install. On XP, no Device Installation Restrictions policy layer exists and
the legacy agent MUST NOT emulate it with class-wide registry deletion. The default XP user-mode
profile therefore uses active per-devnode enforcement plus SCM automatic recovery. An optional machine-wide `USBSTOR` disabled
baseline may be selected only for workstations that never need an approved mass-storage device.
For normal per-device approval, an XP service outage creates a documented fail-open reattachment
window. The console MUST display `legacy_backstop_unavailable`; health and audit data MUST never
report the default XP user-mode profile as fully fail-closed. The experimental filter track
denies **new** USBSTOR attachments while the service is down, but a grant already issued for an
attached disk remains until detach or the service revokes it. It MUST NOT be reported as fully
fail-closed, and its class-wide installation risk requires a recoverable lab image and a completed
XP boot, removal, and physical-I/O test matrix before deployment.

### 6.3 Dry run and safety interlock

`dryRun` defaults to true in developer packages and false only in signed production MSI builds.
Dry run performs discovery, evaluation, registry-plan generation, notification and event logging,
but invokes no mutating Windows or registry API. Live mode requires LocalSystem and a valid signed
configuration. Developer live launches MUST set `dryRun=false` inside the elevated process; a
launcher MUST verify the effective setting rather than assume transient parent-process environment
variables survive Windows elevation. The CLI requires an explicit
`--live --confirm-device-id <id>` pair for mutation.

## 7. Agent lifecycle, storage, and synchronization

Agent files reside under the Windows Common Application Data known folder in `UsbDlp`. Installers
and binaries resolve it through Windows Installer/`SHGetFolderPath`, not an assumed environment
variable. This is normally `%ProgramData%\UsbDlp` on modern Windows and
`C:\Documents and Settings\All Users\Application Data\UsbDlp` on XP. The directory grants full
control to SYSTEM and Administrators only and has inheritance disabled. Users receive no access.
On XP, the installer MUST use an XP-compatible SDDL body without the newer protected-DACL string
token, then disable inheritance with `SetNamedSecurityInfo` and
`PROTECTED_DACL_SECURITY_INFORMATION`. The data volume MUST preserve ACLs (normally NTFS); FAT and
FAT32 installation fails closed with the resolved path, failed operation and native error code.
Legacy wrappers MUST NOT parse localized `reg.exe` output or write path-bearing configuration with
`echo`. They pass quoted input arguments to the native executable, which resolves Common AppData
with `SHGetFolderPathW`, copies files with wide-character APIs, and atomically writes UTF-16LE INI
files. Installation paths containing spaces or characters representable by Windows Unicode,
including Chinese characters, are supported end to end.

- `agent.json`: non-secret configuration, UTF-8 JSON;
- `secrets.dat`: DPAPI LocalMachine-protected API credential and enrollment token;
- `policy.dat`: DPAPI LocalMachine-protected last valid policy;
- `events.db`: SQLite queue and enforcement ownership state; and
- `agent.log`: rolling structured log, 10 x 20 MiB by default.

Modern profiles use DPAPI LocalMachine as described above. Legacy profiles use DPAPI where the
required operation is available and additionally authenticate every persisted policy with an
agent-specific HMAC key. The legacy implementation MUST run only on NTFS, reject writable ACLs for
ordinary users, and fail closed when credential, policy, or queue integrity validation fails.

`agent.json` contains server URL, one or more certificate pins, poll interval, log level, dry-run,
UI culture override, IT contact text, and external-policy mode. Secrets MUST never be logged or
stored in MSI logs. MSI public properties MUST NOT carry the enrollment secret; deployment writes
it through a restricted bootstrap file or post-install enrollment command, which is deleted after
successful import.

At startup the service loads configuration, validates ACLs, loads cached policy or fail-closed
policy, applies the backstop, starts the notification pipe broker, enumerates and enforces, then
begins sync loops. The graphical notifier is not an always-visible part of service startup.
A clean-shutdown marker is cleared at startup and written only during orderly stop. Its absence on
next start produces `tamper_hint` with reason `unexpected_shutdown`; the agent MUST NOT claim
which user stopped it. Registry drift is detected with `RegNotifyChangeKeyValue` and reported as
`tamper_hint` with reason `registry_drift`, without attributing an actor.

Policy polling defaults to 5 minutes with uniform +/-60-second jitter. HTTP 200 validates and
persists the new policy before activation; 304 retains the current policy. Invalid policy never
replaces a valid cache. Offline operation enforces the cache indefinitely. With no valid cache,
the effective policy is schema version 1, version 0, `default_deny`, and no entries.

The event upload loop runs every 60 seconds or when 50 queued events exist. Backoff is exponential
from 5 seconds to 15 minutes with jitter. The queue is WAL-mode SQLite and retains at least 100,000
events or 1 GiB. Uploaded events are purged after 7 days. At the limit, uploaded rows are removed
first; if still full, the agent preserves security/lifecycle/error events over `allowed` events and
emits one `queue_overflow` event when capacity returns. Acceptance requires lossless operation for
at least seven offline days at 10,000 events/day.

## 8. Notifier and IPC

The modern WPF notifier and legacy native Win32 notifier are separate processes only because
Windows services cannot safely display UI in an interactive user session. Each is logically part
of the agent experience, has no policy or
enforcement authority, and is single-instance per Windows session. It MUST NOT show an idle
"waiting for a device" window and need not run continuously. The agent launches it on demand when
a blocked device is observed. If the user closes or terminates it, the agent launches it again for
the next blocked insertion. The process starts hidden, connects to the pipe, and shows a window
only after receiving a current blocked-device message.

On Vista and later, the legacy agent capability-loads `WTSQueryUserToken` to launch the notifier
in the active console session. The XP binary MUST NOT statically import that Vista-only API. On XP,
LocalSystem enumerates `explorer.exe`, accepts only a process whose session ID equals the active
console session, opens it with query-only process access, duplicates its user token, and launches
the notifier on `winsta0\default`. Failure to obtain a matching token is logged and leaves
enforcement active; the service MUST NOT launch UI in session 0 or under LocalSystem identity.

The service hosts multiple instances of `\\.\pipe\UsbDlpNotify` in message mode. The pipe ACL
permits SYSTEM full control and Authenticated Users connect/read/write. On connection, the service
obtains and impersonates the client token, derives user SID and session ID, and accepts a
registration only when those values match the notifier handshake. Client-supplied usernames are
never trusted.

Messages are UTF-8 JSON framed by pipe message boundaries, schema version 1, maximum 32 KiB.
Unknown message types are rejected. Connections have a 10-second handshake timeout and at most one
registered notifier per session. Requests are limited to 5 per user/device/hour.

```json
{"schemaVersion":1,"type":"register","sessionId":2,"notifierVersion":"1.0.0"}
```

```json
{"schemaVersion":1,"type":"blocked","eventId":"uuid","device":{"instanceId":"...","friendlyName":"..."},"reasonCode":"default_deny","allowRequest":true}
```

```json
{"schemaVersion":1,"type":"request_access","eventId":"uuid","deviceInstanceId":"...","requestedDurationDays":30,"justification":"..."}
```

The service routes a block message to the interactive session whose user was captured for the
event. If none exists, the event remains auditable but no toast is shown. Justification is required,
trimmed, 10-2000 characters. Requested duration is one of 1, 7, 30, or 90 days and is advisory;
the approving administrator chooses the actual expiry.

The initial notification MUST show device name, reason, instance ID, `Dismiss`, and
`Request access`. The request action opens the supplied request-dialog design. Success, validation
failure, service unavailable, duplicate pending request, and server unavailable states MUST be
localized. Requests accepted while the server is offline remain durably queued.

The notifier MUST remain connected while its window is open. If another in-scope device is
inserted, it updates all displayed device details and binds submission to the newest blocked event;
stale details MUST never be submitted. A successful request submission dismisses the window
automatically after brief success feedback. A repeated request for the same workstation, user and
canonical device within the server cooldown (10 minutes by default, configurable from 5-15
minutes) is not submitted again; the UI clearly states that a recent request already exists and
shows when another request may be sent. The button is disabled only with that visible explanation
or when required validation is incomplete.

While open, the notifier consumes `allowed` updates. Approval dismisses stale blocked state and
reports that access is available; the agent enables the device on the policy poll when safely
owned. Revocation produces a new policy version, and the agent blocks a currently attached device
on the first successful policy/local reconciliation and blocks it again on every later insertion.

## 9. Server data model

All IDs are UUIDs. All timestamps are UTC. Optimistic concurrency tokens protect mutable entities
in both persistence implementations.

- `Group`: id, unique name, mode, created/updated timestamps.
- `Workstation`: id, unique machine SID, hostname, group ID, API-key salt/hash, key version,
  agent version, OS version, architecture, implementation profile, legacy-risk state,
  administrator description, last seen, applied policy version, enrollment and revoked timestamps.
  Hostname, machine SID and description have normalized searchable columns.
- `UsbDevice`: id, identity kind (`serial` or `workstation_instance`), identity confidence
  (`strong` or `local`), normalized identity hash, VID, PID, nullable serial, friendly name,
  manufacturer, model, description, class/transport/capabilities, first-seen, last-seen and
  created/updated timestamps. Manufacturer/model are observed values; description is
  administrator-maintained and MUST NOT be overwritten by agent ingest.
- `WorkstationDevice`: id, workstation ID, USB-device ID, normalized local instance ID, hardware
  and installation IDs, first/last seen, last event/decision/reason/policy version, last connected
  state and timestamp. The active pair is unique by workstation and normalized instance ID.
- `DeviceEntry`: fields in section 5.2 plus added-by and created/updated timestamps.
- `AccessRequest`: id, client request ID, workstation ID, user SID/name, complete device snapshot,
  USB-device ID, workstation-device ID, requested duration, justification,
  pending/approved/denied/cancelled/revoked status, decision reason, decided-by/at, and resulting
  entry ID.
- `Event`: server ID, unique workstation/client event ID pair, event type, timestamp, received-at,
  workstation/user/device snapshot, decision fields, detail JSON.
- `PolicyVersion`: monotonically increasing version, schema version, published-at/by, reason and
  immutable canonical JSON snapshot.
- `AdminUser`: id, normalized unique username, Argon2id password hash, admin/viewer role, enabled,
  failed-login count, lockout and timestamps.
- `AuditLog`: append-only id, actor, action, target type/ID, timestamp, correlation ID, before/after
  redacted JSON and source IP.
- `EnrollmentToken`: id, SHA-256 hash of 256-bit random token, created/expires/revoked timestamps,
  maximum uses and use count.

`UsbDevice` and `WorkstationDevice` are the operational inventory. `AccessRequest` and `Event`
remain immutable workflow/audit evidence and MUST NOT be used as the primary inventory read model.
Accepted device events and access requests transactionally upsert the device and pair before their
audit row is committed. Replayed client IDs MUST NOT change first-seen timestamps or create duplicate
inventory rows; a later observation MAY refresh mutable observed metadata and last-seen fields.

A trustworthy non-empty hardware serial forms a strong identity from normalized transport, VID,
PID and serial. A missing, substituted or location-derived serial forms only a workstation-local
identity from workstation ID and normalized canonical instance ID. Local identities MUST NOT be
automatically merged across workstations, even when VID, PID, manufacturer and model match. An
administrator-assisted merge is out of scope for v1. Identity hashes are deterministic SHA-256
hashes over normalized server-computed fields and are not accepted from agents.

Policy definitions and effective access are deliberately distinct. A `DeviceEntry` MAY still apply
globally, to a group, or to a workstation. For any `WorkstationDevice`, the server evaluates the
current compiled policy against its latest snapshot to produce a pair-specific `effectiveDecision`,
matched entry, reason, expiry and policy version. Request-generated approvals and direct
administrator grants are workstation-scoped entries linked to the pair. They use strong
VID/PID/serial matching when available and exact instance/install-ID matching for local identities.

The UI MUST report policy state separately from observed enforcement state. Policy state is
`allowed` or `blocked`, optionally qualified by `pending_request`, `approval_expired`, or
`approval_revoked`. Observed enforcement is `allowed`, `blocked`, `pending_change`, or `unknown`,
and includes `lastObservedAtUtc`. A workstation not seen for 10 minutes or a pair with no event for
24 hours is visibly `stale`; the console MUST NOT claim that stale/offline hardware is currently
connected or enforced. A policy newer than the pair's last observed policy is `pending_change`.

Database constraints MUST enforce enum values, required match fields, unique event idempotency,
unique active machine SID, inventory identity uniqueness, pair uniqueness, and referential
integrity. Search indexes cover normalized device ID/VID/PID/serial/name/manufacturer/model/
description and workstation ID/hostname/machine SID/description. Production PostgreSQL uses its
native full-text/trigram indexes; SQLite uses indexed exact/prefix fields plus FTS5 for text. Search
queries MUST NOT scan device JSON. Deleting referenced entities uses restrict or soft-delete;
audit logs and policy snapshots are never cascade-deleted.

## 10. Agent API

Exact request and response DTOs are normative in `usb-dlp-api-contract.md`. If summary text here
differs from that contract, this architecture document controls product behavior and the API
contract MUST be corrected before implementation.

All endpoints are `/api/v1`, HTTPS-only, `application/json`, and return RFC 9457 Problem Details
on error with `traceId`. Request correlation uses `X-Correlation-ID`. Server time is authoritative.

Enrollment:

- `POST /agent/enroll`, max 16 KiB, accepts token, hostname, machine SID, agent version, platform
  profile and a client nonce. Enrollment schema 1 remains accepted for migration; schema 2 requires
  the platform fields. A valid request creates or re-enrolls the same non-revoked machine and returns
  workstation ID, credential, server UTC and policy poll interval. Re-enrollment rotates and
  invalidates the prior key. The nonce is idempotent for 10 minutes.
- Credentials use `<workstation-uuid>.<base64url-32-random-bytes>`. The UUID locates the row; only
  a salted SHA-256 hash of the random secret is stored. Comparison is constant-time.
- Limit is 5 failed enrollments per source IP per 15 minutes and 10 per token per hour.

Authenticated endpoints use `Authorization: Bearer` and update `lastSeenUtc` no more than once
per minute:

- `GET /agent/policy`: returns `CompiledPolicy`; ETag is quoted policy version; matching
  `If-None-Match` returns 304 with no body.
- `POST /agent/events`: body `{schemaVersion, events[]}`, 1-500 events, max 1 MiB. Returns
  accepted and duplicate client IDs plus `policyChanged`; accepted device events upsert inventory.
- `POST /agent/requests`: one request, max 16 KiB, idempotent by client request ID. Returns request
  ID and status. At most one pending request per workstation, user SID and canonical device. The
  request references the inventory device/pair created or refreshed in the same transaction.
- `POST /agent/heartbeat`: agent version, applied policy version, health state, queue depth and
  last enforcement error; returns server UTC and `policyChanged`.

Agent timestamps more than 24 hours in the future are rejected; older timestamps are accepted and
preserved. Hostname is 1-63 characters; usernames 1-256; friendly names 0-200. APIs never accept
scope IDs not belonging to the authenticated workstation context.

Enrollment records `osVersion`, `architecture`, and `implementationProfile`. The server validates
the declared profile against later heartbeats and exposes legacy status in inventory. Modern and
legacy server implementations MUST pass the same black-box API conformance suite. A database may
move from a legacy server to a same-or-newer modern schema through the documented migration tool;
moving a modern database backward into a legacy server is prohibited.

## 11. Policy publication and rollback

Every DeviceEntry or Group mutation is transactional: validate, write mutation and audit log,
increment the global policy version, generate canonical per-workstation documents, snapshot them,
then commit. Compiler output includes global, assigned-group and workstation entries only.

Revoking an approved access request soft-deletes/deactivates its generated allow entry, marks the
request `revoked`, writes an audit record, and publishes a new policy version in the same
transaction. Revocation is not merely a UI status change.

Rollback republishes a selected historical configuration as a new monotonically increasing policy
version; version numbers are never reused or decremented. Concurrent admin edits use optimistic
concurrency and return a visible conflict instead of last-write-wins.

## 12. Admin authentication and UI

Cookie authentication uses Secure, HttpOnly, SameSite=Lax cookies, 30-minute idle and 8-hour
absolute timeout. Login is rate-limited to 10 failures per user/IP per 15 minutes with a 15-minute
lockout. Passwords are at least 14 characters and hashed with Argon2id using parameters recorded
with each hash. CSRF validation is mandatory for mutations. Viewer is read-only at both UI and
handler/service layers.

The first administrator is created by a one-time `UsbDlp.Server admin create` command reading the
password interactively or from a protected standard-input secret. No default credentials exist.
Changing role, resetting passwords, rotating enrollment tokens, and all policy/request/group/user
mutations create audit records.

### 12.1 Visual system and responsive behavior

The supplied screenshots define typography, spacing, neutral warm-gray surfaces, blue selection and
primary actions, green allow, red block, amber pending, rounded cards, thin borders and light theme.
Implementations MUST preserve this visual language; Bootstrap defaults are scaffolding, not the
visual authority. Icons MUST have accessible names or be decorative. Keyboard navigation, visible
focus, WCAG 2.1 AA contrast, 200% zoom, and reduced-motion preferences are required.

Admin pages target 1280 px and wider. From 768-1279 px the sidebar collapses to icons and tables
scroll horizontally. Below 768 px a drawer replaces the sidebar and tables use stacked rows. No
control may become inaccessible. WPF dialogs scale correctly at 100%, 150%, and 200% DPI.

### 12.2 Required screens and states

The console includes Login, Dashboard, Devices, Policies, Workstations, Requests, Events, and
Settings. Dashboard/Policies/Workstations/Requests/Events match screenshots 1-5. Devices uses the
same visual system. Settings follows the same shell and contains Groups, Enrollment,
Administrators, Server health, retention and audit export.

Devices and Workstations are the primary operational navigation. Both provide server-side search,
stable sort, filters and pagination. A unified query searches exact UUID/instance/SID values first,
then case-insensitive tokenized text. Device fields are ID, instance ID, VID, PID, serial, friendly
name, manufacturer, model and description. Workstation fields are ID, hostname, machine SID and
description. Query text is trimmed, 2-200 characters except that exact IDs may be one character or
longer; all terms must match within one result. Results show matching-field context without unsafe
HTML highlighting.

Device detail shows identity confidence and every observed workstation pair. Workstation detail
shows every observed USB device. Each pair shows effective policy decision/source/expiry, request
qualification, observed enforcement and freshness, latest event, and compact request history.
Administrators can create a time-bounded or non-expiring pair grant, revoke an active pair grant,
or open the originating request. Viewers can search and inspect but cannot mutate. Direct grant and
revoke operations publish a new policy version and append an audit record. Requests remains an
approval inbox and historical audit view; administrators never need to scan it to determine the
current status of a device or workstation.

Required dialogs include add/edit/delete policy, change group, add workstation override, approve
request, deny request with required reason, revoke approved access, rotate/revoke token,
create/disable admin and destructive confirmation. Every page/dialog requires loading, empty,
validation, authorization, stale-data
conflict and server-error states. Success/error feedback is announced accessibly and never relies
on color alone. Tables define pagination (25/50/100), stable sort, filters and no-results state.
The policy editor exposes a localized `Mandatory global block` control only for global block
entries, includes an explanation that lower scopes cannot override it, and shows a distinct
non-color-only badge in policy tables.

The Events CSV is UTF-8 with BOM, RFC 4180 quoting, invariant ISO timestamps, stable documented
columns, and spreadsheet-formula injection protection by prefixing dangerous cell values.

### 12.3 Localization

Every visible string, validation message, enum label, CSV heading, notification and accessibility
label uses `.resx`. Supported cultures are en-US and zh-CN; en-US is fallback. Admin culture uses
a cookie; notifier culture uses Windows UI culture unless the service sends a configured override.
Resource-key parity is enforced in CI. Device/user-provided text is never translated.

## 13. Events, audit, and retention

Event types are `blocked`, `allowed`, `access_request`, `policy_applied`, `agent_start`,
`agent_stop`, `enforce_error`, `tamper_hint`, `queue_overflow`, and
`external_policy_conflict`. Every event has a client UUID, occurred-at UTC, hostname, user SID/name
when known, canonical device snapshot when applicable, policy/evaluator result, agent version and
structured details.

Server event retention defaults to 400 days and is configurable from 30-2555 days. Audit logs and
policy snapshots default to 7 years. Purge is daily, batched and audited. CSV export is available;
PDF generation is out of scope. Monthly SEMI E187-oriented reporting is produced from exports.

Logs MUST redact API keys, enrollment tokens, passwords, cookies and justification text by default.
Justification remains available only in the authorized Requests and audit views.

The agent supports an administrator-controlled `DebugLog` option, disabled by default in production
and enabled in external qualification packages. Diagnostic mode records configuration state without
secret values, USB candidate enumeration and exclusion reasons, available canonical identifiers,
policy decisions, Config Manager status/result codes, disable attempts and post-action verification,
and enrollment/policy/heartbeat failures. It MUST NOT record enrollment-token, API-key, password,
cookie, or request-justification contents. Local diagnostic logs rotate at 5 MiB and retain one
previous file. Repeated polling MUST log state transitions rather than duplicate unchanged device
inventory on every cycle.

## 14. Transport and application security

TLS 1.2 or newer is required. The agent validates normal certificate chain and hostname first, then
requires the leaf or SPKI SHA-256 to match one of 1-3 configured pins. Pin updates support overlap:
deploy old+new pins before certificate rotation, rotate certificate, then remove the old pin.
Pin failure never falls back to insecure TLS.

Modern profiles use the .NET/Kestrel TLS implementation and Windows certificate validation.
Legacy profiles MUST use a vendored portable TLS implementation rather than XP/Windows 7 SChannel.
It MUST support TLS 1.2, authenticated encryption, SNI, hostname validation, a package-owned CA
bundle, SPKI SHA-256 pinning, secure renegotiation behavior, and cryptographically secure random
generation. SSLv2, SSLv3, TLS 1.0/1.1, RC4, 3DES, static RSA key exchange, compression and insecure
fallback are prohibited. The dependency must have a documented patch owner, reproducible source
build, software-bill-of-materials entry, vulnerability monitoring, and a successful current CVE
review at every release. If a current critical fix cannot be built and tested for XP/Windows 7,
the affected legacy release MUST be blocked rather than shipped with an old TLS library.

An XP or Windows 7 server MUST bind its management/API listener only to an explicitly configured
interface, default to loopback, and be placed behind an allowlisted firewall or modern reverse
proxy. Installation requires `ACCEPT_LEGACY_SERVER_RISK=YES`, records the acceptance in local and
server audit logs, and displays a persistent warning banner. Legacy server administration from a
browser on the same legacy OS is unsupported; use a current browser on a managed workstation.

Server limits headers to 32 KiB, applies endpoint body limits, uses parameterized SQL/prepared
statements, security headers and anti-forgery. Secrets come from protected configuration or
protected-file providers. Database
backups are encrypted, restored quarterly in a test environment, and retained per customer policy.
Dependency and package vulnerability scanning runs in CI and release builds.

Agent, notifier, CLI and MSI are Authenticode-signed where the target OS can validate the selected
signature. Legacy packages include a signed SHA-256 release manifest verified by the installation
wrapper before MSI execution. No SHA-1-only signature is considered a security guarantee. The
service ACL permits start/query to normal users only if customer policy requires it;
stop/change/config/delete are Administrators/SYSTEM.
SCM recovery restarts after 5, 15 and 60 seconds, then every 5 minutes, resetting after one day.

## 15. Deployment, upgrade, and removal

WiX v4 MSI supports modern silent GPO/Intune/SCCM deployment, per-machine installation and major upgrades.
Configuration validates HTTPS URL and pin format before service start. Upgrade preserves identity,
policy, queue and ownership state and supports one-version rollback. Schema migrations are
transactional and backed up before migration.

Legacy packages use an XP-compatible MSI toolchain and Windows Installer schema, contain no
managed custom actions or bootstrapper prerequisite downloads, and are built separately for x86
and x64. The complete package matrix is agent/server pairs for XP SP3 x86, Windows 7 SP1 x86,
Windows 7 SP1 x64, Windows 10 x86, and Windows 10 x64. Each MSI has a distinct architecture/profile
identity and launch condition. Windows 11 x64 and Windows Server x64 reuse the modern x64 line.

For each workstation/server profile, release automation creates one restricted ZIP deployment kit:

```text
RuisiUsbDlp-<profile>-<version>/
  config/deployment.ini
  config/enrollment-token.txt
  config/admin-password.txt
  client/RuisiUsbDlpAgent-<profile>.msi
  client/install-client.cmd
  server/RuisiUsbDlpServer-<profile>.msi
  server/install-server.cmd
  SHA256SUMS
```

There are two MSIs because server and workstation lifecycles remain independent. The `.cmd`
scripts use only command facilities present on the target OS; PowerShell wrappers MAY also be
included on modern profiles. `deployment.ini` contains editable non-secret server URL, listener,
admin username and profile settings. The two secret files contain exactly one UTF-8/ASCII line and
are byte-identical across kits produced for the same controlled deployment. The administrator MUST
replace the example password before installation. Release kits containing secrets are encrypted or
distributed through an access-controlled software channel and MUST NOT be published as ordinary
download artifacts.

`install-client.cmd` passes secret file paths, never secret values, selects the exact package
profile, and installs with `DRYRUN=false` and `DEFAULTPOLICY=deny`. `install-server.cmd` reads the
same configuration, requires explicit legacy risk acceptance on XP/Windows 7, and imports the
administrator password and enrollment token through protected files. Both scripts stop before MSI
execution for placeholders, missing files, wrong architecture, non-NTFS state, invalid lengths or
an unsafe server URL. MSI logs and process command lines MUST contain no password or token value.

The workstation and server are separate packages with distinct product/upgrade identities,
install directories, services and ProgramData roots. The workstation MSI MUST NOT contain or start
the administration server. The server MSI MUST NOT install the workstation agent, notifier, CLI,
device-control policy or enforcement backstop. Installing, upgrading or removing either product
MUST NOT modify the other product.

The modern server MSI installs the self-contained ASP.NET Core application and the legacy server
MSIs install the native compatibility application, each as the auto-start LocalSystem service
`UsbDlp.Server` with SCM recovery enabled. Binaries are installed under Program Files;
the SQLite database and bootstrap material are stored under an Administrators/SYSTEM-only
`Ruisi\UsbDlpServer` directory beneath the Windows Common Application Data known folder.
Administrator passwords and enrollment tokens MUST
be supplied through protected source files and MUST NOT be passed as MSI property values or written
to MSI logs. The installer copies those files into the protected data directory before service
start. A fresh installation fails before mutation when either source file is absent. The supported
installation wrapper additionally rejects empty input, an administrator password shorter than 14
characters, or an enrollment token shorter than 32 characters before invoking Windows Installer.

The development server package listens on `http://127.0.0.1:5091` by default and marks insecure
HTTP explicitly. A non-loopback listener and Domain/Private firewall exception require an explicit
operator choice. Production deployments MUST use HTTPS with an enterprise certificate, either at
Kestrel or a trusted reverse proxy; no installer option may silently downgrade HTTPS. Server major
upgrade preserves the database and protected configuration without requiring the original staging
files. Uninstall removes the service, binaries and installer-created firewall rule, but preserves
the database, audit state and protected server configuration by default; a separate explicit purge
operation may remove the complete data directory.

Uninstall removes binaries, service, notifier launch support and agent-owned registry policy. ProgramData is
preserved by default for audit/reinstall; explicit `PURGEDATA=1` removes it after stopping service
and removing only agent-owned policy. Uninstall MUST NOT remove external GPO/MDM values.

Server deployment includes reverse-proxy guidance, health endpoints (`/health/live` and
`/health/ready`), backup/restore, certificate/pin rotation, database migration and rollback
runbooks.

## 16. Acceptance criteria

1. On a default-deny workstation, an unapproved USB stick is disabled within 1 second in 95% of
   100 insertions and within 2 seconds in all insertions on reference hardware; event appears within
   90 seconds and localized notification appears within 3 seconds.
2. A workstation-scoped approval with 30-day expiry makes the device usable within one successful
   policy poll; it is blocked no later than the first poll after expiry.
3. Seven days offline at 10,000 events/day preserves cached enforcement and all queued events, which
   upload idempotently after reconnection.
4. Fresh install with no server/cache blocks storage and WPD while keyboard, mouse, webcam, audio,
   internal disks, hubs and controllers remain usable.
5. Android and iOS devices exposing MTP/PTP are blocked in default-deny mode; charge-only mode is
   unaffected.
6. A blocked multi-volume USB disk exposes no usable partition or drive letter. After agent restart,
   current policy is reconciled; after stopping the service, a newly inserted unapproved device is
   denied by the installation backstop on profiles that report that capability. XP instead reports
   `legacy_backstop_unavailable` and is tested against the documented service-down limitation.
   External enterprise policy remains byte-for-byte unchanged. A preinstalled never-evaluated
   device is documented as the accepted residual case.
7. Admin and notifier workflows are complete in en-US and zh-CN, match the supplied visual system,
   and pass keyboard, DPI, zoom and resource-parity checks.
8. Policy evaluation is deterministic and has at least 95% branch coverage, including every scope,
   specificity, tie, expiry boundary, normalization and safety-allow case.
9. No agent operation can disable an internal disk or safety-allowed devnode in automated planning
   tests and the signed VM hardware matrix.
10. API authentication, idempotency, limits, authorization, CSRF, lockout, secret redaction,
    certificate rotation and audit mutation coverage pass integration/security tests.
11. Device and workstation searches return normalized inventory without scanning request/event
    JSON; device/workstation detail reports every observed pair with accurate effective policy,
    observed enforcement and freshness. Direct pair grants and revocations converge through normal
    policy publication and are fully audited.
12. Every release-qualified XP, Windows 7 and Windows 10 OS/architecture profile passes its native
    install, server, agent, notifier, enforcement, API, TLS, upgrade and removal gates. Cross-profile
    policy decisions and API behavior are conformant, and every legacy limitation/risk state is
    visible rather than represented as equivalent to a modern supported host.
