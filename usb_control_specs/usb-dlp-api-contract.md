# USB Device Control - Agent API Contract

Version 1.2. This document is normative and supplements `usb-dlp-spec.md` sections 5,
8, 9, 10, and 13.

## 1. Common conventions

Base path is `/api/v1`. JSON is UTF-8, camelCase, and rejects duplicate property names, invalid
UTF-8, trailing content, unknown enum values, and non-UTC timestamps. Unknown object properties
are ignored. IDs are lowercase canonical UUID strings on output; inputs are case-insensitive.

Every request may include `X-Correlation-ID` as a UUID. The server creates one when absent and
returns it. Authenticated requests use `Authorization: Bearer <workstation-id>.<secret>`.

The JSON contract is implementation-neutral. `modern` .NET and native `legacy` clients and servers
MUST accept and produce the same canonical payloads and pass the same black-box conformance suite.
All profiles use HTTPS with TLS 1.2 or newer. Legacy profiles use the portable TLS requirements in
the architecture specification and MUST NOT negotiate a weaker protocol because the host OS is old.

Errors use `application/problem+json`:

```json
{
  "type": "https://usb-dlp.example/problems/validation",
  "title": "Request validation failed",
  "status": 400,
  "detail": "One or more fields are invalid.",
  "instance": "/api/v1/agent/events",
  "traceId": "00-...",
  "errors": {"events[0].occurredAtUtc": ["A UTC timestamp is required."]}
}
```

Standard status codes are 400 malformed/validation, 401 missing/invalid agent credential, 403
revoked workstation or invalid enrollment token, 409 state/idempotency conflict, 413 body too
large, 415 media type, 422 supported JSON with unsupported schema semantics, 429 rate limit, and
500 unexpected failure. A 429 response includes `Retry-After`.

## 2. Shared DTOs

`DeviceSnapshot`:

```json
{
  "instanceId": "USB\\VID_0781&PID_5583\\4C530001230904",
  "hardwareIds": ["USB\\VID_0781&PID_5583"],
  "deviceInstallIds": ["USB\\VID_0781&PID_5583\\4C530001230904", "USB\\VID_0781&PID_5583"],
  "vid": "0781",
  "pid": "5583",
  "serial": "4C530001230904",
  "classGuid": "{eec5ad98-8080-425f-922a-dabf3de3f69a}",
  "service": "WpdFs",
  "friendlyName": "SanDisk Ultra USB 3.0",
  "manufacturer": "SanDisk",
  "model": "Ultra USB 3.0",
  "description": "USB Mass Storage Device",
  "transport": "usb",
  "capabilities": ["mtp"]
}
```

`instanceId` is required (1-200). `hardwareIds` contains 0-32 and `deviceInstallIds` contains
0-64 unique values. Optional strings serialize as null when unknown. VID/PID match
`^[0-9A-F]{4}$`; serial is 1-128; friendly name, manufacturer, model and description are each
0-200. The server treats observed metadata as searchable attributes, not physical identity.
`transport` is `usb`; capabilities are unique values from `usb_storage|mtp|ptp`.

`AgentHealth` is `healthy|degraded|fail_closed|external_policy_conflict`.
Persisted `RequestStatus` is `pending|approved|denied|cancelled|revoked`. The request-creation
response may additionally use transient status `recent` to report cooldown deduplication; `recent`
is not stored as the prior request's status.

## 3. Enrollment

`POST /api/v1/agent/enroll`, anonymous, 16 KiB maximum:

```json
{
  "schemaVersion": 2,
  "enrollmentToken": "base64url-secret",
  "hostname": "WS-ENG-042",
  "machineSid": "S-1-5-21-...",
  "agentVersion": "1.0.0",
  "osVersion": "5.1.2600-sp3",
  "architecture": "x86",
  "implementationProfile": "legacy-xp-x86",
  "clientNonce": "uuid"
}
```

Enrollment schema 1 remains accepted during migration and omits the three platform fields; the
server records its profile as `unknown` until a schema-2 re-enrollment. Schema 2 requires all fields.
Hostname is 1-63, machine SID 1-184 and must parse as a Windows SID, version is SemVer 2.0, and
nonce is UUID. `osVersion` is 1-64 printable ASCII characters, `architecture` is `x86|x64`, and
`implementationProfile` is one of the release-qualified profile identifiers in the architecture
specification. A profile/architecture mismatch returns 422. Success is 201 for first enrollment or
200 for idempotent replay/re-enrollment:

```json
{
  "schemaVersion": 2,
  "workstationId": "uuid",
  "apiCredential": "uuid.base64url-secret",
  "serverTimeUtc": "2026-08-18T12:00:00.000Z",
  "policyPollSeconds": 300
}
```

The response `schemaVersion` matches the accepted enrollment schema: 1 for a schema-1 migration
client and 2 for a schema-2 client.

Replaying the same token/machine SID/nonce within 10 minutes returns the same response and does not
rotate twice. A different nonce for the same active machine rotates the credential. Token, nonce,
and credential are never returned by any later endpoint.

## 4. Policy

`GET /api/v1/agent/policy` has no body. Success 200 is the exact `CompiledPolicy` contract in
spec section 5.2, with `Content-Type: application/json`, `ETag: "<policyVersion>"`, and
`Cache-Control: no-store`. Matching `If-None-Match` returns 304 with ETag and no body.

## 5. Events

`POST /api/v1/agent/events`, 1 MiB and 1-500 events:

```json
{
  "schemaVersion": 1,
  "events": [{
    "clientEventId": "uuid",
    "type": "blocked",
    "occurredAtUtc": "2026-08-18T12:00:00.000Z",
    "hostname": "WS-ENG-042",
    "userSid": "S-1-5-21-...",
    "username": "DOMAIN\\l.wang",
    "device": {},
    "policyVersion": 42,
    "decision": "block",
    "matchedEntryId": null,
    "reasonCode": "default_deny",
    "agentVersion": "1.0.0",
    "details": {}
  }]
}
```

`type` uses spec section 13. User and device may be null for lifecycle events. Decision is
`allow|block|null`. Details is an object with at most 32 properties and 8 KiB serialized; it MUST
already be secret-redacted. Success 200:

```json
{
  "schemaVersion": 1,
  "acceptedClientEventIds": ["uuid"],
  "duplicateClientEventIds": [],
  "policyChanged": false,
  "currentPolicyVersion": 42
}
```

The batch is atomic for validation: one invalid event rejects all. Replayed valid IDs are reported
as duplicates and are successful. IDs already owned by another workstation produce 409.

## 6. Access requests

`POST /api/v1/agent/requests`, 16 KiB:

```json
{
  "schemaVersion": 1,
  "clientRequestId": "uuid",
  "occurredAtUtc": "2026-08-18T12:00:00.000Z",
  "userSid": "S-1-5-21-...",
  "username": "DOMAIN\\l.wang",
  "device": {},
  "requestedDurationDays": 30,
  "justification": "Flash firmware to tomorrow's test board."
}
```

Duration is `1|7|30|90`; justification is trimmed 10-2000. Success 201, or 200 for replay:

```json
{"schemaVersion":1,"requestId":"uuid","status":"pending","createdAtUtc":"2026-08-18T12:00:01.000Z","retryAfterUtc":"2026-08-18T12:10:01.000Z"}
```

An identical `clientRequestId` replay returns 200 and the original response. Any request created
for the same workstation, user SID and canonical instance within the preceding 10-minute cooldown
returns 200 with the existing `requestId`, status `recent`, original `createdAtUtc`, and
`retryAfterUtc`; it does not create another row. The notifier MUST present this as an existing
recent request rather than a submission error. The production cooldown MAY be configured from
5-15 minutes, but the server-provided `retryAfterUtc` is authoritative.

## 7. Heartbeat

`POST /api/v1/agent/heartbeat`, 16 KiB:

```json
{
  "schemaVersion": 1,
  "agentVersion": "1.0.0",
  "appliedPolicyVersion": 42,
  "health": "healthy",
  "queueDepth": 0,
  "lastEnforcementErrorCode": null,
  "sentAtUtc": "2026-08-18T12:05:00.000Z"
}
```

Queue depth is 0-100000; error code is null or 1-100. Success 200:

```json
{
  "schemaVersion": 1,
  "serverTimeUtc": "2026-08-18T12:05:01.000Z",
  "policyChanged": false,
  "currentPolicyVersion": 42
}
```

## 8. Administrator inventory API

Administrator endpoints use the authenticated admin cookie and the same Problem Details and
correlation conventions. Viewer and administrator roles may read; only administrators may mutate.
Mutation handlers require anti-forgery validation.

- `GET /api/v1/admin/search?q={text}&kind=all|device|workstation&page=1&pageSize=25`
- `GET /api/v1/admin/devices?q={text}&page=1&pageSize=25&sort=lastSeen|name|manufacturer|model`
- `GET /api/v1/admin/devices/{deviceId}`
- `PUT /api/v1/admin/devices/{deviceId}/description`
- `GET /api/v1/admin/workstations?q={text}&page=1&pageSize=25&sort=lastSeen|hostname`
- `GET /api/v1/admin/workstations/{workstationId}`
- `PUT /api/v1/admin/workstations/{workstationId}/description`
- `GET /api/v1/admin/workstations/{workstationId}/devices/{deviceId}`
- `POST /api/v1/admin/workstations/{workstationId}/devices/{deviceId}/grants`
- `DELETE /api/v1/admin/workstations/{workstationId}/devices/{deviceId}/grants/{entryId}`

Page size is `25|50|100`. Collection responses contain `items`, `page`, `pageSize`, `totalCount`
and normalized `query`. Unknown sort/kind values return 400. Detail responses return 404 for a
missing or soft-deleted resource. Search result text is plain data and never contains markup.
Description updates accept `{ "description": "..." }`, where the value is null or trimmed to
1-500 characters. They return 204, do not publish a policy version, and always append an audit row.

Pair detail contains `workstation`, `device`, latest local instance/install IDs,
`effectiveDecision`, `matchedEntryId`, `reasonCode`, `expiresAtUtc`, `policyVersion`,
`requestQualification`, `observedEnforcement`, `lastObservedAtUtc`, `freshness`, `latestEvent`,
and recent requests. `freshness` is `current|stale|offline`; `observedEnforcement` is
`allowed|blocked|pending_change|unknown`. These values follow spec section 9 and are calculated at
read time from current policy plus the latest observation.

Grant request:

```json
{"expiresAtUtc":"2026-09-22T12:00:00.000Z","reason":"Approved engineering transfer"}
```

`expiresAtUtc` is null or a future UTC timestamp no more than 365 days ahead; reason is trimmed
10-1000. Success is 201 and returns the created `entryId`, new `policyVersion`, and refreshed pair
status. An equivalent active grant returns 409. Delete returns 204, soft-deactivates only the named
active grant, publishes a new policy version, and is idempotent only when an idempotency key is
replayed. Direct grants never alter historical access-request status.

## 9. Compatibility and conformance

Clients MUST send schema version 1. Server supports the current and immediately previous agent
minor version. Unsupported schema returns 422 with `supportedSchemaVersions`. New optional fields
may be added within schema version 1; removing fields, changing meaning/type, or adding enum values
requires schema version 2.

Integration tests MUST serialize requests with the production JSON options and compare successful
responses against checked-in golden JSON. OpenAPI generated by the server MUST match this contract;
CI fails on an unreviewed OpenAPI diff.
