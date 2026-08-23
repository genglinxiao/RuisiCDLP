# USB Device Control — Implementation Prompts (TDD, staged)

Sequenced prompts for a coding agent (a lesser model than the architect). The work is
grouped into five **stages**. Each stage ends at a **human verification gate** — a point
where you (a person) can build, run, and confirm real behavior before the agent proceeds.
Within each stage, every milestone follows **red → green → refactor**: the agent writes
failing tests first, then the minimum code to pass, then cleans up.

Companion documents: `usb-dlp-spec.md` (normative architecture),
`usb-dlp-api-contract.md` (normative wire contract), and `usb-dlp-test-plan.md` (the full
automated + manual test plans that these gates reference).

Stage order and dependencies:

```
Stage A  Core policy logic         (M0, M1)      → GATE A  (pure logic, no Windows/VM)
Stage B  Server + admin UI         (M6, M7)      → GATE B  (run on any machine, browser)
Stage C  Agent device layer        (M2, M3)      → GATE C  (Windows VM, dry-run first)
Stage D  Agent integration         (M4, M5)      → GATE D  (VM + server, end-to-end)
Stage E  Notifier + installer      (M8)          → GATE E  (clean VM, full deploy)
```

---

## TDD protocol — prepend to EVERY milestone prompt

```
Work test-first (red → green → refactor):
1. RED: write the tests named in this milestone's "Tests to write first" list. Run them.
   They MUST fail (or not compile) for the right reason. Paste the failing output.
2. GREEN: write the minimum production code to make those tests pass. Do not add behavior
   the tests don't require. Run the full suite; paste the passing output.
3. REFACTOR: clean up names/duplication with tests staying green. Paste the final green run.
Do not proceed to the next milestone until the current one is fully green.
Do not delete or weaken a test to make it pass. If a test is wrong, explain why, then fix it.
Every test has an ID (e.g. T-M1-03) — keep these IDs as test method names or [Trait] tags
so they map to usb-dlp-test-plan.md.
```

## Engineering rules — also prepend to EVERY prompt

```
You are implementing part of a Windows USB device-control system in C# / .NET 8.
- Frameworks: net8.0 for shared/server, net8.0-windows for agent/notifier.
- User-mode ONLY. No kernel driver, no C++. Windows APIs via P/Invoke.
- Do NOT invent architecture. `usb-dlp-spec.md` is normative. If it is ambiguous, stop and
  request a specification decision; do not encode unspecified behavior.
- All user-facing strings in .resx (en-US default + zh-CN). Never hard-code a display string.
- Never touch HID, Keyboard, Mouse, SmartCardReader, Printer, USB hub/controller classes —
  a hard-coded allowlist server policy cannot override.
- Secrets come from config/env, never source literals.
- Output complete, compiling files. No "// rest here" placeholders.
```

---

# STAGE A — Core policy logic

No Windows APIs, no VM. This is the correctness keystone; it must be rock-solid before anything else.

## M0 — Solution scaffold + CI test loop

```
Tests to write first:
- T-M0-01: a trivial xUnit test in UsbDlp.Tests that asserts true, proving the test runner
  is wired to CI and `dotnet test` fails the build on a failing test.

Create a .NET 8 solution `UsbDlp` with projects:
UsbDlp.Shared (classlib net8.0), UsbDlp.Windows (classlib net8.0-windows),
UsbDlp.Agent (worker, net8.0-windows), UsbDlp.Notifier (WPF, net8.0-windows),
UsbDlp.Server (ASP.NET Core Razor Pages, net8.0), UsbDlp.Cli (console, net8.0-windows),
UsbDlp.Tests and UsbDlp.IntegrationTests (xUnit, net8.0). Keep Windows dependencies out of Shared.
Enable nullable + treat-warnings-as-errors on Shared. Add a README with build/test/run steps
and a `dotnet test` command that CI will run.
```
Acceptance: `dotnet build` + `dotnet test` succeed; flipping T-M0-01 to assert false fails the run.

## M1 — Shared model + policy matcher (the keystone)

```
Tests to write first (all in UsbDlp.Tests, against PolicyEvaluator.Evaluate):
- T-M1-01: device whose class is in the hard-coded allowlist returns Allow even when a
  Block class-entry also matches (allowlist wins).
- T-M1-02: exact instance-id Allow beats a vid+pid Block for the same device (specificity).
- T-M1-03: within the same specificity tier, a Block and an Allow both matching → Block.
- T-M1-04: an entry with ExpiresAtUtc in the past is ignored.
- T-M1-05: vid+pid entry (serial null) matches a device with any serial (wildcard serial).
- T-M1-06: no matching entry + GroupMode.DefaultDeny → Block.
- T-M1-07: no matching entry + GroupMode.DefaultAllow → Allow.
- T-M1-08: VID/PID matching is case-insensitive (0781 vs 0781, ABCD vs abcd).
- T-M1-09: expiry boundary — ExpiresAtUtc exactly == nowUtc is treated as expired.
- T-M1-10 through T-M1-13: order independence, field validation, mandatory-global plus
  workstation/group/global scope precedence, and complete decision metadata.

Then implement in UsbDlp.Shared:
enums MatchType{InstanceId,VidPidSerial,VidPid,DeviceClass}, EntryAction{Allow,Block},
GroupMode{DefaultDeny,DefaultAllow}, ScopeType{Global,Group,Workstation}, Decision{Allow,Block};
records DeviceEntry, DeviceInfo, CompiledPolicy (see spec §4.3/§5);
HardCodedSafetyAllowList and product device classes `usb_storage` / `wpd` exactly as spec §4-5;
static Decision PolicyEvaluator.Evaluate(DeviceInfo, CompiledPolicy, DateTime nowUtc)
implementing spec §5.3 precedence exactly.
Target ≥95% branch coverage on PolicyEvaluator; add a coverage step to `dotnet test`.
```
Acceptance: all T-M1-* green; coverage report ≥95% on the evaluator.

### ▶ GATE A — human verification (see test-plan §A)
You run `dotnet test`, read the printed T-M1-* results, and eyeball the coverage number.
Run `UsbDlp.Cli evaluate` with sample device and policy JSON and print the complete evaluation
result to hand-check representative device IDs.
**Sign off before Stage B.**

---

# STAGE B — Server + admin UI

Runs on any machine; verified through a browser. No VM needed yet.

## M6 — Server data model, agent API, policy compiler

```
Tests to write first (integration tests using WebApplicationFactory + EF Core SQLite in-memory):
- T-M6-01: POST /api/v1/agent/enroll with a valid token returns a workstationId + apiKey;
  the apiKey is stored only as a hash.
- T-M6-02: enroll with a bad/expired token is rejected (401/403) and rate-limited after N tries.
- T-M6-03: GET /api/v1/agent/policy with a valid Bearer key returns the compiled policy for
  that workstation; a global entry appears, another workstation's override does NOT.
- T-M6-04: GET policy with matching If-None-Match returns 304.
- T-M6-05: POST /api/v1/agent/events twice with the same client GUID stores one row (idempotent).
- T-M6-06: changing a DeviceEntry bumps PolicyVersion and updates the compiled output.
- T-M6-07: PolicyCompiler includes global + group + workstation-scoped entries and the group
  mode, and excludes other groups' entries.
- T-M6-08 through T-M6-12: limits/Problem Details, credential rotation, enrollment idempotency,
  request constraints, transactional publication and monotonic rollback.
- T-M6-13 through T-M6-20: inventory upsert/idempotency, strong versus local identity isolation,
  JSON-free indexed search, pair detail status/freshness, legacy backfill, direct grant and revoke.

Then implement EF Core entities (spec §9) + migrations, including UsbDevice and
WorkstationDevice operational inventory; the agent REST API (spec §10)
with Bearer-key middleware, rate limiting, max body size, idempotent event ingest; and
PolicyCompiler (spec §5.3) that snapshots each version. SQLite by default, connection-string
switch to PostgreSQL.
```
Acceptance: all T-M6-* green.

## M7 — Admin UI (Razor Pages, light theme, en + zh-CN)

```
Tests to write first:
- T-M7-01: unauthenticated request to any admin page redirects to login.
- T-M7-02: a viewer-role user gets 403 (or hidden controls) on any mutating action.
- T-M7-03: approving an AccessRequest creates a scoped Allow DeviceEntry with the chosen
  scope + expiry and marks the request approved (test the handler/service directly).
- T-M7-04: every admin mutation writes an AuditLog row.
- T-M7-05: a resource-key presence test — for each key in en-US .resx, the same key exists
  in zh-CN .resx (fail CI on any missing/extra key). No hard-coded strings in .cshtml
  (scan for literal text outside localizer calls).
- T-M7-06: requesting a page with culture=zh-CN renders the zh-CN string for a known key.
- T-M7-07 through T-M7-10: auth/cookie/CSRF security, safe CSV, required UI states, and
  accessibility/zoom behavior.
- T-M7-11 through T-M7-15: device/workstation search and detail, pair status, viewer read-only
  behavior, direct grant/revoke auditing, and safe no-results/pagination behavior.

Then build the pages matching `screenshots/en` and `screenshots/zh` (EN + ZH): Login (cookie auth,
argon2), Dashboard, Devices (search + detail + workstation pairs), Policies (layered table +
add/edit modal w/ wildcard serial + expiry), Workstations (search + detail + observed devices),
Requests (approve→scoped allow / deny+reason),
Events (filter + safe CSV export), Settings (groups, token rotation, admin users, health,
retention and audit export). Implement every state and responsive rule in spec §12. CSRF on all
forms; culture via CookieRequestCultureProvider + top-bar switcher; all strings via .resx.
```
Acceptance: all T-M7-* green.

### ▶ GATE B — human verification (see test-plan §B)
You run the server locally, log in, click through all six pages **in English and Chinese**,
create a group + a couple of device entries, approve a seeded request, export a CSV, and
confirm the language switcher flips every label. Verify a viewer account can't mutate.
**Sign off before Stage C.**

---

# STAGE C — Agent device layer (Windows VM)

First code that touches hardware. Test on a throwaway Windows 11 VM. Enforcer runs
**dry-run first**. Device P/Invoke can't be fully unit-tested, so tests cover the pure
parts and a manual checklist covers the rest.

## M2 — Device monitor + enumerator (P/Invoke)

```
Tests to write first (unit-testable pure logic only):
- T-M2-01: instance-id parser extracts VID/PID/serial from
  "USB\VID_0781&PID_5583\4C530001230904" → vid=0781, pid=5583, serial=4C530001230904.
- T-M2-02: parser on an id with a Windows-substituted &-prefixed token sets serial=null.
- T-M2-03: parser handles WPD/MTP-style ids and lowercase hex without throwing.
- T-M2-04 through T-M2-07: WPD parent resolution, internal/safety exclusion, event
  deduplication, and registration-before-enumeration/resume behavior.

Then implement DeviceMonitor (spec §4): register for all device-instance notifications, enqueue
callbacks, debounce and enumerate all present setup classes, filter by USB ancestry/service/WPD
capability, and resolve child devnodes to one canonical physical identity. WMI is only a wake-up
fallback. This class only observes — no enforcement.
Provide a console harness `UsbDlp.Cli monitor` that prints arriving devices, for manual gate testing.
```
Acceptance: T-M2-* green; harness prints correct DeviceInfo when you plug in a stick (gate C).

## M3 — Enforcer (P/Invoke, two layers)

```
Tests to write first:
- T-M3-01: registry-policy builder produces the documented layered installation-policy plan from
  a CompiledPolicy and never emits a forbidden setup class (test the pure planner, not writes).
- T-M3-02: ClearRegistryPolicy targets only keys/values this app tagged as its own (given a
  simulated key state, it removes ours and leaves unrelated policy untouched).
- T-M3-03: in dry-run mode, Disable/Enable/ApplyRegistryPolicy log intended actions and make
  no real changes (inject an abstraction over the CM_*/registry calls and assert no calls).
- T-M3-04 through T-M3-08: forbidden-class planning, safe disable ownership and status recheck,
  external-policy preservation/conflict, and layered removable-deny/observed-allow-ID mapping.

Then implement Enforcer (spec §6): non-persistent CM disable, exact agent ownership required
before enable, periodic status verification for cached Block decisions, forbidden-class safety
interlock, and a registry planner that never denies the
generic Disk/input/hub/controller classes, never writes RemovableStorageDevices access denial,
and never changes external GPO/MDM values. Config bool
DryRun defaults true outside signed production MSI.
```
Acceptance: T-M3-* green.

### ▶ GATE C — human verification (see test-plan §C) — DO THIS ON A THROWAWAY VM
1. Run `UsbDlp.Cli monitor`, plug/unplug a USB stick and a phone (MTP) → correct DeviceInfo prints.
2. With **DryRun=true**, feed a default-deny policy → logs say it *would* disable the stick;
   Device Manager shows the stick still working. (Confirms no accidental bricking.)
3. Only after (2) looks right, set **DryRun=false** → the unapproved stick shows disabled in
   Device Manager; an allow entry re-enables it; keyboard/mouse never affected.
4. Apply then ClearRegistryPolicy → confirm no residual keys under the two policy paths.
**Sign off before Stage D.** (This gate is why the registry path is dangerous — verify by hand.)

---

# STAGE D — Agent integration (VM + server, end-to-end)

## M4 — Sync client, policy cache, event queue

```
Tests to write first:
- T-M4-01: cert-pin callback accepts the configured SHA-256 and REJECTS a different cert.
- T-M4-02: FetchPolicy on 304 keeps the cached policy; on 200 replaces + persists it.
- T-M4-03: PolicyCache round-trips a CompiledPolicy through DPAPI-protected JSON on disk.
- T-M4-04: no cache + server unreachable → GetEffectivePolicy returns the FAIL-CLOSED policy
  (DefaultDeny, empty entries).
- T-M4-05: EventQueue persists events to SQLite, survives a simulated restart, and upload
  marks them by client GUID without duplicating (idempotent).
- T-M4-06: enroll stores apiKey + workstationId and removes the enroll token from config.
- T-M4-07 through T-M4-09: DPAPI/redaction, seven-day queue load, and overlapping pin rotation.

Then implement SyncClient, DPAPI `secrets.dat` and `policy.dat`, bounded WAL EventQueue,
redaction, fail-closed fallback, overlapping TLS pins, and ACL validation per spec §7 and §10.
```
Acceptance: all T-M4-* green (T-M4-04 fail-closed is the safety-critical one).

## M5 — Agent worker host (wire it all together)

```
Tests to write first (host logic with mocked DeviceMonitor/Enforcer/SyncClient):
- T-M5-01: DeviceArrived + Block decision → Enforcer.Disable called + 'blocked' event
  enqueued + notifier message sent.
- T-M5-02: DeviceArrived + Allow decision → no Disable + 'allowed' event enqueued.
- T-M5-03: policy version change on poll → ApplyRegistryPolicy re-run + all present devices
  re-evaluated.
- T-M5-04: unclean previous shutdown → 'tamper_hint' emitted on next start.
- T-M5-05 through T-M5-09: token-derived pipe identity, IPC limits, durable offline request,
  Block status revalidation, multi-volume replug and revocation convergence.

Then implement the BackgroundService (LocalSystem, auto-start): startup load (config → cached/
fail-closed policy → apply registry → enumerate + enforce), device-event handling, poll loop,
event upload loop, and authenticated/session-routed named-pipe server (spec §8). Emit lifecycle
events. Service recovery is set by the installer (M8), not code.
```
Acceptance: all T-M5-* green.

### ▶ GATE D — human verification (see test-plan §D) — VM + running server
1. Install/run the agent on the VM pointed at your Stage-B server; confirm it enrolls and
   appears online in the Workstations tab with the current policy version.
2. Plug an unapproved stick → blocked within ~1 s; a `blocked` event appears in the Events
   tab within ~90 s with correct user/host/device.
3. From the console, approve access scoped to that workstation → within one poll the stick works.
4. Pull the VM's network → cached policy still enforces; reconnect → queued events upload, none lost.
5. Fresh agent, server unreachable → storage blocked, keyboard/mouse fine (fail-closed).
**Sign off before Stage E.**

---

# STAGE E — Notifier + installer (full deployment)

## M8 — Notifier tray app + WiX MSI

```
Tests to write first:
- T-M8-01: notifier resource-key parity (en-US ↔ zh-CN) like T-M7-05.
- T-M8-02: pipe-message (de)serialization round-trips a 'blocked' message and a
  'request_access' message.
- T-M8-03 (installer smoke, scripted): MSI install creates the service (auto-start, recovery=
  restart) and permits on-demand notifier launch into the active session; uninstall removes them.
- T-M8-04 through T-M8-07: all localized notifier states, secret-free MSI logs, state-preserving
  upgrade/rollback, and ownership-safe uninstall/purge.

Then build UsbDlp.Notifier from screenshot 6 and spec §8: on-demand hidden startup, per-session
single instance, reconnect/relaunch behavior, live device replacement, authenticated pipe
registration, request dialog, cooldown/success/approval states, and DPI/accessibility behavior in
both locales. Then the WiX MSI: install service (auto-start, recovery=restart) and notifier,
write non-secret agent.json settings, ACL ProgramData to SYSTEM+Admins, and import enrollment
secret only from a restricted bootstrap file or post-install command without MSI property/log
exposure. Authenticode-sign MSI + binaries (standard OV/EV cert; NO Microsoft driver
attestation — there is no kernel driver). Document signing + GPO/Intune/SCCM deployment.
```
Acceptance: T-M8-* green.

### ▶ GATE E — human verification (see test-plan §E) — clean Win11 VM
Deploy the MSI; confirm service auto-starts, notifier shows a **localized** toast on block,
Request access reaches the console and round-trips to an approval, and uninstall cleans up.
Run the full end-to-end acceptance list in spec §8 one last time in both languages.
**Final sign-off = release candidate.**

---

## Notes for whoever drives the agent
- Give one milestone at a time. Require the red output before accepting green — that's how you
  know the tests actually exercise the code.
- M1 and M4-04 (fail-closed) and M3 (registry) are the three places to review personally.
- If the agent tries to skip a failing device-API test by mocking away the behavior under test,
  reject it — the pure logic must stay genuinely tested; only the OS calls get abstracted.
