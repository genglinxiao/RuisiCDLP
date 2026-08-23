# USB Device Control - Agent API Contract

Version 1.0. This document is normative and supplements `usb-dlp-spec.md` sections 5,
8, 9, 10, and 13.

## 1. Common conventions

Base path is `/api/v1`. JSON is UTF-8, camelCase, and rejects duplicate property names, invalid
UTF-8, trailing content, unknown enum values, and non-UTC timestamps. Unknown object properties
are ignored. IDs are lowercase canonical UUID strings on output; inputs are case-insensitive.

Every request may include `X-Correlation-ID` as a UUID. The server creates one when absent and
returns it. Authenticated requests use `Authorization: Bearer <workstation-id>.<secret>`.

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
  "transport": "usb",
  "capabilities": ["mtp"]
}
```

`instanceId` is required (1-200). `hardwareIds` contains 0-32 and `deviceInstallIds` contains
0-64 unique values. Optional strings serialize
as null when unknown. VID/PID match `^[0-9A-F]{4}$`; serial is 1-128; friendly name 0-200.
`transport` is `usb`; capabilities are unique values from `usb_storage|mtp|ptp`.

`AgentHealth` is `healthy|degraded|fail_closed|external_policy_conflict`.
Persisted `RequestStatus` is `pending|approved|denied|cancelled|revoked`. The request-creation
response may additionally use transient status `recent` to report cooldown deduplication; `recent`
is not stored as the prior request's status.

## 3. Enrollment

`POST /api/v1/agent/enroll`, anonymous, 16 KiB maximum:

```json
{
  "schemaVersion": 1,
  "enrollmentToken": "base64url-secret",
  "hostname": "WS-ENG-042",
  "machineSid": "S-1-5-21-...",
  "agentVersion": "1.0.0",
  "clientNonce": "uuid"
}
```

Hostname is 1-63, machine SID 1-184 and must parse as a Windows SID, version is SemVer 2.0, and
nonce is UUID. Success is 201 for first enrollment or 200 for idempotent replay/re-enrollment:

```json
{
  "schemaVersion": 1,
  "workstationId": "uuid",
  "apiCredential": "uuid.base64url-secret",
  "serverTimeUtc": "2026-08-18T12:00:00.000Z",
  "policyPollSeconds": 300
}
```

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

## 8. Compatibility and conformance

Clients MUST send schema version 1. Server supports the current and immediately previous agent
minor version. Unsupported schema returns 422 with `supportedSchemaVersions`. New optional fields
may be added within schema version 1; removing fields, changing meaning/type, or adding enum values
requires schema version 2.

Integration tests MUST serialize requests with the production JSON options and compare successful
responses against checked-in golden JSON. OpenAPI generated by the server MUST match this contract;
CI fails on an unreviewed OpenAPI diff.
