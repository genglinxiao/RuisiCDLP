# USB Device Control — Test Plan

Companion to `usb-dlp-spec.md`, `usb-dlp-api-contract.md`, and
`usb-dlp-implementation-prompts.md`. Two layers:

1. **Automated tests** — written test-first by the coding agent, run in CI on every commit.
   Each has a stable ID (e.g. `T-M1-03`) that matches the test method name / trait.
2. **Manual gate tests** — performed by a human at the end of each stage on the exact
   OS/architecture VMs where hardware is involved. A stage is not "done" until its gate is signed off.

Environments:
- **Dev/CI** — any machine; runs all automated tests (`dotnet test`).
- **VM-MODERN-X86/X64** — fresh Windows 10 22H2 x86/x64 VMs.
- **VM-WIN7-X86/X64** — fresh Windows 7 SP1 x86/x64 VMs with customer-approved prerequisite updates.
- **VM-XP-X86** — fresh Windows XP Professional SP3 x86 VM on an isolated test network.
- **VM-WIN11-X64** — fresh Windows 11 x64 safety/reference VM.
- **SRV-TEST** — the Stage-B server running locally or on a test host, reachable from all matrix VMs.
- **SRV-LEGACY** — native server installed in turn on VM-XP-X86 and VM-WIN7-X86/X64.

Definition of done per stage = all automated tests for that stage green **and** the manual
gate checklist fully passed and signed (name + date below each gate).

---

## Automated test matrix

| ID | Area | Asserts | Type |
|----|------|---------|------|
| T-M0-01 | CI loop | test runner fails build on a failing test | unit |
| T-M1-01 | Matcher | hard-coded class allowlist wins over a Block entry | unit |
| T-M1-01a | Matcher | USBSTOR disk below a safety-class USB hub remains default-denied; the hub itself is never selected for enforcement | unit / XP integration |
| T-M1-02 | Matcher | instance-id Allow beats vid+pid Block (specificity) | unit |
| T-M1-03 | Matcher | Block beats Allow within one specificity tier | unit |
| T-M1-04 | Matcher | expired entry is ignored | unit |
| T-M1-05 | Matcher | vid+pid entry matches any serial (wildcard) | unit |
| T-M1-06 | Matcher | no match + default-deny → Block | unit |
| T-M1-07 | Matcher | no match + default-allow → Allow | unit |
| T-M1-08 | Matcher | VID/PID compare is case-insensitive | unit |
| T-M1-09 | Matcher | expiry boundary (ExpiresAt == now) treated as expired | unit |
| T-M1-10 | Matcher | policy JSON order does not affect the result | unit |
| T-M1-11 | Matcher | malformed match-field combinations are rejected | unit |
| T-M1-12 | Matcher | mandatory global, workstation, group, global scope matrix | unit |
| T-M1-13 | Matcher | result includes reason, matched entry, version and evaluation time | unit |
| T-M6-01 | Server | enroll returns workstationId+apiKey, key stored hashed | integration |
| T-M6-02 | Server | bad/expired token rejected + rate-limited | integration |
| T-M6-03 | Server | policy fetch returns this ws's compiled policy only | integration |
| T-M6-04 | Server | If-None-Match match → 304 | integration |
| T-M6-05 | Server | duplicate client-GUID event stored once (idempotent) | integration |
| T-M6-06 | Server | entry change bumps PolicyVersion + recompiles | integration |
| T-M6-07 | Compiler | includes global+group+ws scope, excludes other groups | unit |
| T-M6-08 | API | body/batch/string/time limits return Problem Details | integration |
| T-M6-09 | Auth | key rotation invalidates old key; comparisons and lookup are safe | integration |
| T-M6-10 | Enroll | nonce is idempotent; token use/expiry/rate limits enforced | integration |
| T-M6-11 | Requests | duplicate/pending request constraints are idempotent | integration |
| T-M6-12 | Publish | mutation+audit+snapshot are atomic; rollback creates newer version | integration |
| T-M6-13 | Inventory ingest | accepted event/request upserts one device and workstation pair transactionally | integration |
| T-M6-14 | Strong identity | same normalized VID/PID/serial on two workstations resolves to one device and two pairs | integration |
| T-M6-15 | Local identity | no-serial instances on different workstations remain separate devices | integration |
| T-M6-16 | Inventory search | IDs, instance, hostname, SID, name, manufacturer, model and description search normalized columns, not JSON | integration |
| T-M6-17 | Pair status | detail computes effective decision, source, expiry, observed enforcement and freshness | integration |
| T-M6-18 | Migration | existing request/event JSON backfills inventory idempotently without changing audit rows | integration |
| T-M6-19 | Direct grant | admin pair grant creates the correct workstation-scoped entry and publishes/audits | integration |
| T-M6-20 | Direct revoke | revokes only the selected pair grant, republishes and preserves request history | integration |
| T-M7-01 | Admin UI | unauth request redirects to login | integration |
| T-M7-02 | Admin UI | viewer role blocked from mutations | integration |
| T-M7-03 | Admin UI | approve request → scoped Allow entry + status set | unit/integration |
| T-M7-04 | Admin UI | every mutation writes an AuditLog row | integration |
| T-M7-05 | i18n | en-US ↔ zh-CN resource keys match; no hard-coded UI text | unit |
| T-M7-06 | i18n | culture=zh-CN renders zh string for a known key | integration |
| T-M7-07 | Security | login lockout, cookie flags and CSRF are enforced | integration |
| T-M7-08 | CSV | RFC 4180, UTF-8 BOM and formula-injection protection | unit |
| T-M7-09 | UI states | required pages/dialogs expose empty/error/conflict states | browser |
| T-M7-10 | Accessibility | keyboard, focus, contrast and 200% zoom checks pass | browser |
| T-M7-11 | Device UI | searchable/paged device inventory and identity confidence render safely | integration/browser |
| T-M7-12 | Workstation UI | searchable/paged workstations and observed devices render safely | integration/browser |
| T-M7-13 | Pair UI | device/workstation details show effective and observed state with freshness | integration/browser |
| T-M7-14 | Inventory auth | viewer can inspect but direct pair grant/revoke is unavailable and forbidden | integration |
| T-M7-15 | Search safety | invalid paging/sort is rejected and no-results/untrusted text is escaped | integration/browser |
| T-M2-01 | Parser | VID/PID/serial parsed from standard instance id | unit |
| T-M2-02 | Parser | &-substituted token → serial=null | unit |
| T-M2-03 | Parser | WPD/MTP + lowercase hex ids parse without throwing | unit |
| T-M2-04 | Identity | WPD child resolves to canonical USB ancestor identity | unit |
| T-M2-05 | Scope | internal disk and every safety class are excluded | unit |
| T-M2-06 | Monitor | child notifications debounce to one canonical event | unit |
| T-M2-07 | Monitor | registration precedes enumeration; resume triggers enumeration | unit |
| T-M2-08 | Monitor | USBSTOR disk wins over WPD children regardless of enumeration order | unit |
| T-M2-09 | Monitor | multi-volume media produces one canonical device with exact USBSTOR target | Windows integration |
| T-M2-10 | Metadata | Windows snapshot captures manufacturer, model and description with null-safe fallback | unit/Windows integration |
| T-M3-01 | Enforcer | registry value-set built correctly from a policy | unit |
| T-M3-02 | Enforcer | ClearRegistryPolicy removes only our tagged values | unit |
| T-M3-03 | Enforcer | dry-run makes no real CM_*/registry calls | unit |
| T-M3-04 | Safety | registry plan never denies Disk/controllers/hubs/HID/input | unit |
| T-M3-05 | Ownership | agent enables only devnodes it successfully disabled | unit |
| T-M3-06 | Coexistence | external GPO/MDM values are preserved and conflict emitted | unit |
| T-M3-07 | Backstop | layered removable deny + observed allow-ID mapping is deterministic | unit |
| T-M3-08 | Enforcer | owned Block verifies actual devnode status and re-disables an enabled node | Windows integration |
| T-M4-01 | Sync | cert-pin accepts configured cert, rejects others | unit |
| T-M4-02 | Sync | 304 keeps cache; 200 replaces + persists | unit |
| T-M4-03 | Cache | CompiledPolicy round-trips via DPAPI JSON on disk | unit |
| T-M4-04 | Safety | no cache + server down → FAIL-CLOSED policy | unit |
| T-M4-05 | Events | queue durable across restart, idempotent upload | unit |
| T-M4-06 | Enroll | apiKey stored, enroll token removed from config | unit |
| T-M4-07 | Secrets | credentials/cache are DPAPI protected and logs redact secrets | unit |
| T-M4-08 | Queue | seven-day/70k-event offline load uploads without loss | integration |
| T-M4-09 | Pins | overlapping old/new pins support rotation without insecure fallback | unit |
| T-M5-01 | Host | new Block → Disable + one blocked event + notifier msg | unit |
| T-M5-02 | Host | Allow → no Disable + allowed event | unit |
| T-M5-03 | Host | policy change → re-apply registry + re-evaluate all | unit |
| T-M5-04 | Host | unclean prior shutdown → tamper_hint on start | unit |
| T-M5-05 | IPC auth | service derives SID/session from pipe token, not message | Windows integration |
| T-M5-06 | IPC limits | handshake, size, type, duplicate and rate limits enforced | unit |
| T-M5-07 | Request | offline request is durable and requested duration is advisory | unit |
| T-M5-08 | Host | unchanged disabled Block is quiet; re-enabled same ID is blocked again | unit/integration |
| T-M5-09 | Host | revoke publishes policy and blocks an attached approved device | integration |
| T-M8-01 | i18n | notifier resource-key parity en↔zh | unit |
| T-M8-02 | IPC | pipe messages round-trip (blocked, request_access) | unit |
| T-M8-03 | Installer | MSI installs service and on-demand notifier launch support; uninstall removes both | smoke |
| T-M8-04 | Notifier | block, request, success and all error states are localized | UI |
| T-M8-05 | Installer | enrollment secret never appears in MSI properties/logs | smoke |
| T-M8-06 | Upgrade | identity, queue, policy and ownership survive upgrade/rollback | smoke |
| T-M8-07 | Uninstall | external policy preserved; ProgramData purge is explicit only | smoke |
| T-M8-08 | Notifier | starts hidden on demand, relaunches after close, updates for newest device | UI/integration |
| T-M8-09 | Notifier | success auto-dismisses; recent duplicate visibly shows retry time | UI/integration |
| T-M9-01 | Server installer | server MSI contains server/runtime only; agent MSI contains workstation components only | smoke |
| T-M9-02 | Server service | clean install creates auto-start service, recovery actions, protected data directory and healthy endpoints | Windows smoke |
| T-M9-03 | Server secrets | protected source files install successfully; missing/empty/short inputs fail without secrets in MSI logs | security smoke |
| T-M9-04 | Server network | default listener is loopback; LAN exposure and Domain/Private firewall rule require explicit switches | Windows smoke |
| T-M9-05 | Server upgrade | major upgrade preserves database, admin access, enrollment state, policies, inventory and audit | Windows integration |
| T-M9-06 | Server uninstall | service/binaries/firewall removed; protected state retained unless explicit purge is requested | Windows smoke |
| T-M9-07 | Product isolation | server install/upgrade/uninstall does not modify agent service, files, state or enforcement and vice versa | Windows integration |
| T-M10-01 | Contract parity | modern and every legacy profile produce identical policy decisions and canonical JSON from shared vectors | native/.NET unit |
| T-M10-02 | Legacy build | native binaries import no CLR and no Win32 API newer than their declared minimum OS | static analysis |
| T-M10-03 | Profile guard | each MSI accepts only its exact OS/architecture and rejects all others before mutation | VM smoke |
| T-M10-04 | Legacy TLS | XP/7 agent and server negotiate TLS 1.2 AEAD only; bad chain/name/pin and downgrade fail closed | security integration |
| T-M10-05 | Legacy storage | NTFS/ACL/integrity checks pass; FAT or user-writable state is rejected | VM security |
| T-M10-06 | XP enforcement | unapproved multi-volume USBSTOR is disabled; approval/revocation converges; safety devices are untouched | XP hardware integration |
| T-M10-06a | XP incomplete identity | a USBSTOR/WPD devnode with temporarily unavailable VID/PID remains governed and default-deny blocks it; diagnostics report `vid=unknown`/`pid=unknown` | XP hardware integration |
| T-M10-06b | XP remote diagnostics | `DebugLog=1` records candidate inclusion/exclusion, decision, disable result and verification without secrets or unchanged-poll flooding; `DebugLog=0` suppresses diagnostic records | XP integration |
| T-M10-06c | XP data ACL | installer protection succeeds on localized XP/NTFS using an XP-compatible SDDL DACL plus protected-inheritance API; resulting ACL contains only non-inherited SYSTEM and Administrators full control; FAT/FAT32 fails with an explicit diagnostic | XP integration |
| T-M10-06d | XP Common AppData resolution | wrappers never invoke or parse `reg.exe`; each native binary resolves the known folder with `SHGetFolderPathW` and reports its effective data directory | XP integration |
| T-M10-06e | XP Unicode paths | native configuration resolves Common AppData without batch/registry parsing and round-trips UTF-16 configuration under paths containing spaces and Chinese characters | XP integration |
| T-M10-07 | XP limitation | stopped agent reports/retains `legacy_backstop_unavailable` and never claims full fail-closed health | XP integration |
| T-M10-08 | Win7 enforcement | x86/x64 active enforcement and capability-tested installation backstop match declared health | Win7 hardware integration |
| T-M10-09 | Legacy server | XP/7 native server passes API, admin, persistence, backup/restore and audit conformance | VM/browser integration |
| T-M10-10 | Legacy server risk | install fails without explicit risk acceptance; listener defaults loopback and warning is persistent | VM security smoke |
| T-M10-11 | Cross-profile | every agent profile enrolls/syncs/requests against every server profile | matrix integration |
| T-M10-12 | Dependency gate | legacy TLS/SQLite SBOM, reproducible source build and current vulnerability review pass | release security |
| T-M10-13 | Deployment kits | every profile ZIP has two isolated MSIs, two compatible scripts, editable config/secrets, hashes, live mode and default deny | package smoke |

CI gate: the build fails if any automated test fails, if evaluator branch coverage < 95%,
or if resource-key parity (T-M7-05 / T-M8-01) finds a missing/extra key.

---

## Manual gate tests

Record result (Pass/Fail), tester, date, and notes under each. Revert the VM snapshot
between destructive runs.

### GATE A — core logic (Dev/CI)
- [ ] A1 `dotnet test` all green; T-M1-* visibly listed.
- [ ] A2 Evaluator branch coverage ≥ 95% (read the coverage report).
- [ ] A3 Using `UsbDlp.Cli evaluate` on 3 hand-picked device+policy cases (one allow, one
      block, one no-match-under-deny), the printed Decision matches your manual expectation.

Signed: __________ Date: ______

### GATE B — server + admin UI (SRV-TEST, browser)
- [ ] B1 Log in as admin; all seven navigation destinations load in the light theme, matching the mockups and visual system.
- [ ] B2 Switch language EN↔中文 — every visible label changes; no leftover English/Chinese,
      no missing-key placeholders.
- [ ] B3 Create a group (default-deny), add a global allow entry + a group block entry.
- [ ] B4 Seed/approve an access request → a scoped Allow entry is created with the chosen expiry.
- [ ] B5 Events tab filters by type/date; CSV export opens with correct columns.
- [ ] B6 Log in as a viewer → no add/edit/approve controls; direct mutation URL is refused.
- [ ] B7 Every action in B3–B4 shows up in the audit log.
- [ ] B8 Search Devices by UUID, instance ID, VID/PID, serial, name, manufacturer, model and description; verify expected results, pagination and no-results state.
- [ ] B9 Search Workstations by UUID, hostname, machine SID and description; open one and verify all observed USB devices are listed.
- [ ] B10 Open one device observed on multiple workstations; each pair shows effective decision/source/expiry, observed state and freshness without claiming an offline agent is current.
- [ ] B11 Create and revoke a direct pair grant from detail pages; only that workstation/device pair changes, policy version increments each time, and audit records are present.
- [ ] B12 Log in as viewer; inventory remains searchable but pair grant/revoke controls are absent and direct mutation returns 403.

Signed: __________ Date: ______

### GATE B2 — packaged server (clean Windows Server VM)
- [ ] B2.1 Inspect both MSIs: the server package contains no agent/notifier/CLI, and the agent package contains no server executable or server service.
- [ ] B2.2 Install with protected password/token files; `UsbDlp.Server` starts automatically, recovery is configured, both health URLs return 200, and admin login succeeds.
- [ ] B2.3 Verify `%ProgramData%\Ruisi\UsbDlpServer` grants access only to Administrators and SYSTEM; verbose MSI logs contain neither secret value.
- [ ] B2.4 Verify the default endpoint is reachable from the server itself but not another host. Reinstall for an isolated LAN test and verify only the selected TCP port is opened on Domain/Private profiles.
- [ ] B2.5 Enroll an agent and create policy/inventory/audit data, upgrade one version, then verify all state and credentials remain valid.
- [ ] B2.6 Uninstall and verify service, binaries and firewall rule are gone while the database remains. Reinstall and recover state, then uninstall with explicit purge and verify the data directory is gone.
- [ ] B2.7 Repeat install/uninstall while the workstation package is present and verify its service, files, state and active USB enforcement remain unchanged.

Signed: __________ Date: ______

### GATE C — agent device layer (VM-MODERN-X64, snapshot first)
- [ ] C1 `UsbDlp.Cli monitor` prints correct VID/PID/serial/class when you plug in a USB
      stick plus manufacturer/model/description when Windows provides them; serial=null and missing
      metadata cases are handled without invented values.
- [ ] C2 Same for a phone in MTP mode (WPD class detected).
- [ ] C3 **DryRun=true** + default-deny policy: logs say it *would* disable the stick; Device
      Manager shows the stick still functional (nothing actually disabled).
- [ ] C4 **DryRun=false**: unapproved stick shows disabled in Device Manager; adding an allow
      entry re-enables it.
- [ ] C4a On a two-partition/multi-volume USB stick, blocking the physical USBSTOR disk removes
      every partition and drive letter; no WPD child remains usable.
- [ ] C5 Keyboard and mouse remain fully functional throughout C3–C4 (hard-coded allowlist).
- [ ] C6 ApplyRegistryPolicy then ClearRegistryPolicy → no residual agent-owned values under
      DeviceInstall\Restrictions and no writes under RemovableStorageDevices. Revert snapshot.
- [ ] C7 Confirm internal/system disks, hubs, controllers, webcam, audio and input devices were
      never included in the mutation plan.
- [ ] C8 Pre-seed representative domain-policy values; apply/clear UsbDlp and confirm byte-for-byte
      preservation plus an `external_policy_conflict` event.

Signed: __________ Date: ______

### GATE D — agent integration (VM-MODERN-X64 + SRV-TEST)
- [ ] D1 Agent enrolls; workstation appears "online" with current policy version in the console.
- [ ] D2 On reference hardware, run 100 scripted/manual insertion cycles: at least 95 block
      within 1 s and all within 2 s. Each notification appears within 3 s and every `blocked`
      event appears within 90 s with correct user, hostname, device and policy version.
- [ ] D3 Approve access scoped to this workstation → within one poll interval the stick works.
- [ ] D3a Revoke that approval while attached → all volumes disappear on the next reconciliation;
      unplug, wait through one local poll, and replug → the device is blocked again.
- [ ] D4 Disconnect VM network: cached policy still enforces (stick still blocked). Reconnect:
      queued events upload; count before/after confirms none lost.
- [ ] D5 Fresh install with SRV-TEST unreachable → all removable storage blocked, keyboard/
      mouse unaffected (fail-closed).
- [ ] D6 Phone in MTP mode blocked on the default-deny group.
- [ ] D7 Restart the agent and confirm current policy reconverges. Stop the service and confirm a
      never-installed unapproved removable device is denied installation by the backstop;
      `tamper_hint` appears after agent restart.
- [ ] D8 Confirm a manual allow lacking installation IDs is marked active-only, then attach
      observed IDs and confirm `passiveExceptionAvailable=true`.
- [ ] D9 Rotate TLS certificate using overlapping pins without outage; wrong pin remains rejected.

Signed: __________ Date: ______

### GATE E — notifier + full deployment (VM-MODERN-X64, snapshot first)
- [ ] E1 MSI installs; service is present, auto-start, recovery=restart; notifier can be launched
      into the active user session by the service without an always-visible idle window.
- [ ] E2 Block event → notifier toast appears in the session's language (test once EN, once 中文)
      with device name, reason, instance id, and the IT-contact footer.
- [ ] E3 Click "Request access", enter justification → request appears in the console Requests
      tab; approving it lets the device through on next poll.
- [ ] E3a Submit successfully → window dismisses. Reinsert during the 10-minute cooldown → the UI
      names the recent request and retry time instead of showing an unexplained disabled button.
- [ ] E3b Close/kill the notifier, then insert a blocked device → agent relaunches it. While its
      window is open, insert another blocked device → all displayed details update to the new device.
- [ ] E4 Re-run the full spec §8 acceptance list (1–8) end-to-end in both languages.
- [ ] E5 Uninstall removes service, notifier launch support, and (on confirm) the ProgramData folder.
- [ ] E6 Verify notifier at 100/150/200% DPI and admin UI at desktop/tablet/mobile widths.
- [ ] E7 Verify install logs contain no enrollment token and normal uninstall preserves ProgramData.

Signed: __________ Date: ______  → **Pass on all gates = release candidate.**

### GATE F — legacy compatibility matrix (isolated VMs, snapshots required)

Build-host evidence recorded 2026-08-28: all five profile-specific agent/server MSI pairs and
restricted ZIP kits build successfully; native policy and DPAPI tests pass for XP x86 and Windows
10 x86/x64; XP binaries are x86, subsystem 5.01, statically linked, and contain no CLR or
Vista-only `WTSQueryUserToken` import; all kit SHA-256 manifests pass. This cross-build evidence
does not satisfy the unchecked VM and physical-hardware gates below.

XP VM evidence recorded 2026-08-28 on Windows XP Professional SP3 x86 in a Generation 1
Hyper-V VM with an isolated internal network: both profile MSIs and both installation scripts
returned zero; the native policy and DPAPI suites passed; `UsbDlp.LegacyServer` and
`UsbDlp.LegacyAgent` reached `RUNNING`; the agent log reported `dry_run=false`,
`default_deny=true`, successful enrollment, and repeated policy version 1 application; an
authenticated host request to `/api/v1/admin/workstations` returned HTTP 200 and the
`RUISI-XP-LAB` inventory record. This is evidence for the XP VM portions of F1 and F9 only.
It does not close F3-F8: this Hyper-V configuration cannot pass a physical USB device directly
to the guest, and notifier, approval/revocation, service-down, TLS, backup/restore, upgrade,
uninstall, and physical enforcement cases remain unchecked.

- [ ] F1 Build XP x86, Windows 7 x86/x64, and Windows 10 x86/x64 agent/server MSIs and restricted deployment kits; verify exact-profile launch conditions and absence of CLR imports in legacy binaries.
- [ ] F2 Run shared policy/API vectors against every implementation; outputs are byte-canonical where specified and semantically identical everywhere else.
- [ ] F3 On each agent VM, repeat discovery, default deny, request, approval, attached revocation, replug, multi-volume, queue/offline, notifier relaunch, and safety-allow tests.
- [ ] F4 On XP, stop the agent and verify the console reports `legacy_backstop_unavailable`; confirm documentation does not promise the modern service-down backstop.
- [ ] F5 Install each legacy server only with explicit risk acceptance; administer it remotely from a current browser, then exercise enrollment, approval, revoke, inventory, audit, backup, restore and upgrade.
- [ ] F6 Run every agent profile against every server profile. Verify profile/architecture inventory, warning badges, health, policy versions and timestamps.
- [ ] F7 Prove TLS 1.0/1.1, weak ciphers, bad hostnames, bad chains and wrong pins are rejected on XP/7; review the legacy dependency SBOM and unresolved vulnerabilities.
- [ ] F8 Verify legacy packages and release manifests, secret-free logs, ACLs, service recovery, uninstall preservation and explicit purge on all matrix VMs.
- [ ] F9 Run both scripts from each kit after editing only `deployment.ini`, `admin-password.txt`, and `enrollment-token.txt`; verify client live mode/default deny and server login/enrollment without secrets in command lines or logs.
- [ ] F10 On a fresh XP SP3 x86 NTFS snapshot, install the SetupAPI kit only. For each USBSTOR disk record class-installer return, devnode status/problem and actual file read/write results. Run at least 20 replug cycles, a second stick, every LUN of a multi-LUN stick, approval, revocation, service restart and reboot. `blocked_verified` must never coexist with successful unapproved file I/O. Validate exact agent-owned re-enable, rollback with a disconnected owned device (must refuse removal), retry with it connected, and no effect on keyboard, hub, or internal disk.
- [ ] F11 Restore a clean XP SP3 x86 NTFS snapshot, install the filter kit only, and reboot. Confirm XP boots, internal disk and input devices work, filter service is present, and every unapproved USBSTOR volume rejects file reads/writes, raw-disk opens and storage-control requests. Test approval, revocation while attached, replug, multi-LUN, service-down insertion, service restart and driver removal/reboot. Capture blue-screen/minidump and System Event Log on any failure; restore snapshot rather than editing a damaged system live.
- [ ] F12 On both tracks, verify notifier says block is unconfirmed when enforcement cannot be verified; it must not display a false blocked claim. Verify MSI payloads, x86/5.01 PE headers, SHA-256 manifests, server/agent isolation and the documented rollback. Neither track is release-qualified on cross-build evidence alone.
- [ ] F13 **Known failing regression:** 1.1.6 permits file I/O after a local administrator manually enables a denied device, until a later reconciliation disables it again. Time the exposure window and test copying a file during it; a subsequent `blocked_verified` does not pass this test.
- [ ] F14 **Known failing regression:** 1.1.7 logs filter request result 2 for all disks and no `blocked_verified`. On the diagnostic build, run `client\diagnose-filter.cmd` after reboot, collect its full output and the XP System Event Log, then confirm the agent's `stage=` identifies the failure. Do not proceed as protected if the health check fails. Even if it passes, manually enable an unapproved disk in Device Manager and test actual reads/writes on every LUN both before and after replug; any success fails the track.

Signed: __________ Date: ______  → **Legacy packages may ship only after all F gates pass.**

---

## Regression / release checklist (run before any release build)
- [ ] Full modern `dotnet test` and legacy native test suites green on clean checkouts; coverage thresholds met.
- [ ] GATE C, D, E manual checklists re-run on a fresh VM snapshot.
- [ ] GATE F completed on the full legacy OS/architecture matrix.
- [ ] MSI + binaries Authenticode signature verifies (`signtool verify /pa`).
- [ ] Both .resx locales complete (parity tests green).
- [ ] Cert-pin still matches the deployed server certificate (rotate config if the cert changed).
- [ ] Rollback rehearsed: previous PolicyVersion can be re-published and agents converge to it.

---

## System acceptance traceability

The acceptance numbers below refer to `usb-dlp-spec.md` section 16. A release candidate requires
both the automated evidence and every named manual gate; a narrower test does not substitute.

| Acceptance | Automated evidence | Manual evidence |
|---|---|---|
| AC-1 timing/event/notifier | T-M2-06, T-M5-01, T-M8-02, T-M8-04 | D2, E2 |
| AC-2 approval and expiry | T-M1-04, T-M1-09, T-M5-03, T-M7-03 | D3, E3; use a short test expiry to exercise convergence |
| AC-3 seven-day offline queue | T-M4-02, T-M4-05, T-M4-08, T-M6-05 | D4 |
| AC-4 fresh fail-closed safety | T-M1-01, T-M2-05, T-M4-04, T-M5-01 | D5, C5, C7 |
| AC-5 MTP/PTP and charge-only | T-M2-03, T-M2-04, T-M2-05 | C2, D6 |
| AC-6 service-down backstop | T-M3-04, T-M3-05, T-M3-06, T-M3-07 | C8, D7, D8 |
| AC-7 bilingual accessible UI | T-M7-05 through T-M7-10, T-M8-01, T-M8-04 | B1-B6, E2, E6 |
| AC-8 deterministic evaluator | T-M1-01 through T-M1-13 plus 95% branch gate | A1-A3 |
| AC-9 internal/safety device protection | T-M2-05, T-M3-03, T-M3-04 | C3-C5, C7 |
| AC-10 API/application security | T-M4-01, T-M4-07, T-M4-09, T-M6-01 through T-M6-12, T-M7-01 through T-M7-08, T-M8-05 | B6-B7, D9, release checklist |
| AC-11 searchable pair inventory | T-M2-10, T-M6-13 through T-M6-20, T-M7-11 through T-M7-15 | B8-B12 |
| AC-12 legacy compatibility | T-M10-01 through T-M10-13 | F1-F9 |
