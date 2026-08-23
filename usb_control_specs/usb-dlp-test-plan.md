# USB Device Control — Test Plan

Companion to `usb-dlp-spec.md`, `usb-dlp-api-contract.md`, and
`usb-dlp-implementation-prompts.md`. Two layers:

1. **Automated tests** — written test-first by the coding agent, run in CI on every commit.
   Each has a stable ID (e.g. `T-M1-03`) that matches the test method name / trait.
2. **Manual gate tests** — performed by a human at the end of each stage, on a real
   Windows 11 VM where hardware is involved. A stage is not "done" until its gate is signed off.

Environments:
- **Dev/CI** — any machine; runs all automated tests (`dotnet test`).
- **VM-CLEAN** — fresh Windows 11 VM, snapshot before each destructive gate so you can revert.
- **SRV-TEST** — the Stage-B server running locally or on a test host, reachable from VM-CLEAN.

Definition of done per stage = all automated tests for that stage green **and** the manual
gate checklist fully passed and signed (name + date below each gate).

---

## Automated test matrix

| ID | Area | Asserts | Type |
|----|------|---------|------|
| T-M0-01 | CI loop | test runner fails build on a failing test | unit |
| T-M1-01 | Matcher | hard-coded class allowlist wins over a Block entry | unit |
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
| T-M2-01 | Parser | VID/PID/serial parsed from standard instance id | unit |
| T-M2-02 | Parser | &-substituted token → serial=null | unit |
| T-M2-03 | Parser | WPD/MTP + lowercase hex ids parse without throwing | unit |
| T-M2-04 | Identity | WPD child resolves to canonical USB ancestor identity | unit |
| T-M2-05 | Scope | internal disk and every safety class are excluded | unit |
| T-M2-06 | Monitor | child notifications debounce to one canonical event | unit |
| T-M2-07 | Monitor | registration precedes enumeration; resume triggers enumeration | unit |
| T-M2-08 | Monitor | USBSTOR disk wins over WPD children regardless of enumeration order | unit |
| T-M2-09 | Monitor | multi-volume media produces one canonical device with exact USBSTOR target | Windows integration |
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
- [ ] B1 Log in as admin; all six tabs load in the light theme, matching the mockups.
- [ ] B2 Switch language EN↔中文 — every visible label changes; no leftover English/Chinese,
      no missing-key placeholders.
- [ ] B3 Create a group (default-deny), add a global allow entry + a group block entry.
- [ ] B4 Seed/approve an access request → a scoped Allow entry is created with the chosen expiry.
- [ ] B5 Events tab filters by type/date; CSV export opens with correct columns.
- [ ] B6 Log in as a viewer → no add/edit/approve controls; direct mutation URL is refused.
- [ ] B7 Every action in B3–B4 shows up in the audit log.

Signed: __________ Date: ______

### GATE C — agent device layer (VM-CLEAN, snapshot first)
- [ ] C1 `UsbDlp.Cli monitor` prints correct VID/PID/serial/class when you plug in a USB
      stick; serial=null case handled for a stick that reports none.
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

### GATE D — agent integration (VM-CLEAN + SRV-TEST)
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

### GATE E — notifier + full deployment (VM-CLEAN, snapshot first)
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

---

## Regression / release checklist (run before any release build)
- [ ] Full `dotnet test` green on a clean checkout; coverage threshold met.
- [ ] GATE C, D, E manual checklists re-run on a fresh VM snapshot.
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
