# ENDAP Stable RC Public API And Module Split Plan

This document defines the safe path to make ENDAP easier to maintain and easier for external contributors to extend without touching the deterministic kernel.

## Current Verdict

The current architecture is valid for the product base:

- the 1 ms control loop remains small and deterministic;
- node admission, security, dashboard and transport policy live outside the critical loop;
- the dashboard already talks through HTTP APIs instead of directly coupling to internal state;
- transport-aware runtime behavior is already visible through `/api/status`, `/api/profile` and `/api/nodes`.

The main structural problem is concentration, not a wrong product direction. Some files became too large because they carry several responsibilities at once.

## Main Structural Risks

- `components/runtime/http_server/http_server.c` mixes routing, auth guards, JSON building, WebSocket status, node admission, IO configuration, automation and network APIs.
- `components/dashboard/index.html` mixes all dashboard domains in one embedded asset.
- the previous inverted scheduler-to-HTTP component dependency has already been removed; keep this boundary closed in future changes.
- `components/runtime/io_driver/io_driver.c` notifies the HTTP server directly from an output-change hook. The preferred direction is runtime event/flag first, HTTP observation second.
- Several mutating APIs still use `GET`; this is acceptable for the current embedded iteration, but public API compatibility should move critical writes to `POST`.

## Target Module Shape

Keep one embedded HTTP server, but split its implementation by domain:

- `api_status.c`: `/api/status`, WebSocket payload helpers and runtime snapshot JSON.
- `api_auth.c`: `/api/auth/*`, accounts, audit and auth response JSON.
- `api_nodes.c`: `/api/nodes/*`, admission state, profile/template actions and node diagnostics.
- `api_io.c`: `/api/set`, `/api/input/*`, `/api/output/*`.
- `api_network.c`: `/api/network/*`, `/api/wifi/*`, recovery and reboot operations.
- `api_automation.c`: `/api/automation/*`.
- `api_profile.c`: `/api/profile`, `/api/public/profile`, component catalog and expansion readiness.
- `api_json.c` / `api_json.h`: shared JSON append helpers.
- `http_server.c`: server lifecycle, URI registration, WebSocket transport and shared response headers only.

The first split must preserve every endpoint and every response field.

## Public API Compatibility Rules

- Do not remove current endpoints until a compatibility window exists.
- Add `POST` endpoints for mutating operations, then keep current `GET` endpoints as aliases during RC.
- Every public endpoint should return JSON eventually; text responses may remain for legacy aliases.
- Public API fields should be additive. New fields are allowed; renamed fields need aliases.
- Security must remain at the HTTP/API boundary and not enter the deterministic loop.
- Channel identity is split between node-local channel fields and the gateway installation map. Firmware exposes `local_code` such as `OUT1`; the gateway/dashboard may derive `OUT105` or aliases for the installation.

## Stable Public Endpoint Groups

Read-only or mostly read-only:

- `GET /api/status`
- `GET /api/profile`
- `GET /api/public/profile`
- `GET /api/nodes`
- `GET /api/automation`
- `GET /api/auth/status`
- `GET /api/auth/users`
- `GET /api/auth/audit`
- `GET /api/network/preview`
- `GET /api/wifi/status`
- `GET /api/wifi/scan`

Mutating operations to migrate to POST-first:

- `/api/set`
- `/api/nodes/adopt`
- `/api/nodes/configure`
- `/api/nodes/activate`
- `/api/nodes/revoke`
- `/api/network/config`
- `/api/wifi`
- `/api/recovery`
- `/api/reboot`
- `/api/input/*`
- `/api/output/*`
- `/api/automation/add`
- `/api/automation/remove`
- `/api/automation/clear`
- `/api/kernel/load`

## Runtime Boundary Corrections

Priority 1:

- Replace direct `io_driver -> http_server_notify_state_change()` with a runtime notification primitive such as `runtime_notify_state_changed()`.
- Let HTTP/WebSocket observe that primitive outside the hot path.
- Keep the scheduler component free of HTTP/dashboard requirements.

Priority 2:

- Keep `core_boot` as orchestration/composition root, but avoid adding domain logic there.
- Keep `cluster_manager`, `cluster_failover`, `fieldbus`, `scheduler` and `control_loop` closed unless a bug forces a narrow patch.

## Contribution Model

External contributors should be able to add features in these areas without touching the kernel:

- new dashboard sections backed by `/api/*`;
- new profile/component catalog fields;
- new node templates and labels;
- new non-critical diagnostics;
- new transport-specific dashboard status;
- new hardware readiness adapters after real hardware validation.

External contributors should not modify:

- 1 ms loop timing;
- scheduler phase semantics;
- deterministic IO apply path;
- cluster failover core;
- fieldbus timing;
- ISR/IRAM paths.

## Suggested Execution Order

1. Stabilize current dashboard behavior and public JSON fields.
2. Extract shared JSON helpers from `http_server.c`.
3. Split auth APIs into `api_auth.c`.
4. Split node admission APIs into `api_nodes.c`.
5. Split profile/catalog APIs into `api_profile.c`.
6. Move mutating operations to POST-first while keeping GET aliases.
7. Remove inverted runtime-to-HTTP notification dependency.
8. Publish a short API reference for contributors.

## Definition Of Done For The Split

- `idf.py build` passes after each file extraction.
- Current dashboard works without endpoint changes.
- Existing curl workflows still work.
- New POST endpoints exist before old GET aliases are deprecated.
- No deterministic-loop code gains dependency on HTTP, dashboard, auth or network services.
