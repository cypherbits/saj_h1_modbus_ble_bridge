# Improvement backlog — `modbus_ble_bridge`

Working document for the component's improvements found while adding support for
ESPHome 2025.9 → 2026.9 (five compatibility rounds: 2026.5, 2026.6, 2026.7,
2026.8, 2026.9).

**How to use it**

* Work items in the order listed under [Recommended order](#recommended-order), one
  `P#` per change, pausing for review after each one.
* Every item states its own risk, backward-compatibility impact and verification
  method, and is meant to be applied as an **isolated** change.
* When an item is finished, its `Status` changes to `DONE (verified on …)` and the
  [status board](#status-board) is updated.

**Legend**

| Field | Meaning |
| --- | --- |
| `Independent` | Can be applied on its own without any other item. |
| `Backward compat` | Keeps existing YAML configs working unchanged. |
| `Risk` | Chance of breaking runtime behaviour (needs hardware test). |

**Supported range used for every check below**: ESPHome `2025.9.3`, `2025.10.4`,
`2025.11.5`, `2025.12.7`, `2026.1.5`, `2026.2.4`, `2026.3.3`, `2026.4.5`,
`2026.5.3`, `2026.6.5`, `2026.7.4`, `2026.8.2`, `2026.9.1`.
Each round was validated with `esphome config` + `esphome compile` on both test
configs (`tests/test.yaml`, `tests/test_no_metrics.yaml`).

> **Repo state note (outside this backlog):** the 2026.9.1 support changes
> (`.github/workflows/ci.yml` matrix entry + `README.md` version lists) were still
> uncommitted when this file was written.

## Status board

| # | Item | Category | Order | Status |
| --- | --- | --- | --- |--------|
| P5 | De-duplicate the BLE response timeout logic | A. Robustness (C++) | 1 | TODO    |
| P6 | Include concrete headers instead of `esphome.h` | A. Robustness (C++) | 2 | TODO   |
| P8 | Validate `modbus_port` with `cv.port` | B. Configurability | 3 | TODO   |
| P7 | Expose `ble_response_timeout` in YAML | B. Configurability | 4 | TODO   |
| P13 | Fix stale roadmap / document all options | D. Documentation | 5 | TODO   |
| P4 | Read the exact Modbus frame | A. Robustness (C++) | 6 | TODO   |
| P3 | Non-blocking TCP send (TX buffer) | A. Robustness (C++) | 7 | TODO   |
| P9 | `timeout-minutes` on the CI build job | C. CI guards | 8 | TODO   |
| P10 | Fail CI on any component compiler warning | C. CI guards | 9 | TODO   |
| P11 | Assert the conditional `AUTO_LOAD` really skips `sensor` | C. CI guards | 10 | TODO   |
| P12 | Keep CI matrix and README version lists in sync | C. CI guards | 11 | TODO   |
| P2 | Use `parent_->register_for_notify()` (2026.8+) | A. Robustness (C++) | 12 | TODO   |
| P1 | Release the GATT cache (report `ESTABLISHED` + write by handle) | A. Robustness (C++) | 13 | TODO   |

## Recommended order

Lowest risk first; the only item that cannot be signed off without real hardware
is **P1** (and, to a lesser degree, **P3**).

`P5 → P6 → P8 → P7 → P13 → P4 → P3 → P9 → P10 → P11 → P12 → P2 → P1`

---

## A. Component robustness (C++)

### P5 — De-duplicate the BLE response timeout logic
**Status:** DONE — `check_ble_response_timeout_(now, context)` in
`modbus_ble_bridge.cpp/.h`; both call sites collapsed to one line each.
**Independent:** yes (touches only `modbus_ble_bridge.cpp` / `.h`)
**Backward compat:** yes (pure refactor, no YAML or interface change)
**Risk:** very low

**Where:** `components/modbus_ble_bridge/modbus_ble_bridge.cpp`, lines 149-157
(`loop()`) and 192-201 (`handle_modbus_tcp()`).

**Problem:** The same sequence — check `waiting_since_` against
`ble_response_timeout_ms_`, count the timeout, clear `waiting_message_` /
`waiting_since_`, log, close the TCP client — exists twice, in two copies that have
already drifted (`"...timeout... (loop)"` vs `"...timeout..."`). Any future change
to the timeout policy (e.g. P7 making it configurable, or not closing the client)
must be applied in two places, and forgetting one produces inconsistent behaviour
depending on which code path notices the expiry first.

**Proposed change:** extract a private helper
`bool check_ble_response_timeout_(unsigned long now, const char *context)` that
performs the whole sequence and returns `true` when a timeout was handled. Both
call sites keep their surrounding behaviour:

* `loop()` — call it unconditionally (as today), ignore the result.
* `handle_modbus_tcp()` — call it inside the `waiting_message_` branch and keep the
  existing `return;`.

The `context` argument preserves the diagnostic information that the two original
messages carried (`"loop"` / `"while waiting"`), so one log line covers both paths.

**Verification:** compile both test configs; the counters (`note_timeout_()`),
state reset and `close_client_()` calls are unchanged, so behaviour is identical.

**Result:** `esphome config` + `esphome compile` exit 0 for both configs on
`2025.9.3` and `2026.9.1` (oldest and newest supported), with zero `warning:` /
`error:` lines. The two log messages now read
`BLE response timeout after %u ms (loop|while waiting). Resetting state.`, so the
`loop` / `while waiting` context that the duplicated code carried is preserved.

### P6 — Include concrete headers instead of `esphome.h`
**Status:** TODO
**Independent:** yes
**Backward compat:** yes (no interface change)
**Risk:** very low

**Where:** `components/modbus_ble_bridge/modbus_ble_bridge.h`, line 2.

**Problem:** `#include "esphome.h"` pulls in the per-build aggregate header (102
includes in our build) into every translation unit that compiles the component.
It couples the component to unrelated parts of ESPHome, hides which APIs are
actually used, and slows compilation. The real dependencies are few.

**Proposed change:** replace it with the specific headers:
`esphome/core/component.h`, `esphome/core/hal.h` (`millis`, `delay`),
`esphome/core/log.h`, `esphome/components/ble_client/ble_client.h`,
`esphome/components/network/util.h` and, guarded by
`#ifdef USE_MODBUS_BLE_BRIDGE_METRICS`, `esphome/components/sensor/sensor.h`.

**Verification:** the 13-version `config` + `compile` matrix (a missing include is
a hard compile error), plus no change in the generated `defines.h`/`main.cpp`.

### P4 — Read the exact Modbus frame
**Status:** TODO
**Independent:** yes
**Backward compat:** yes (request handling stays the same for complete frames)
**Risk:** low

**Where:** `modbus_ble_bridge.cpp`, `handle_modbus_tcp()` (peek of
`MIN_TCP_FRAME` bytes, then a single `recv(..., MAX_TCP_FRAME, MSG_DONTWAIT)`).

**Problem:** When a client pipelines two requests (allowed by Modbus/TCP), both
frames are consumed by the same `recv()`: the trailing bytes of the second request
are silently discarded and never answered, so the client waits for a timeout. This
is not detected or logged today.

**Proposed change:** read the fixed 12-byte header first, derive the full length
from the MBAP length field (`mbap_len + 6`, clamped to `MAX_TCP_FRAME`), then read
exactly the remaining bytes; if more bytes were consumed than the frame needs, log
it. No change to the protocol handling itself.

**Verification:** compile both configs; a pipelined-request test needs a TCP client
against the real device (or at least a careful code review of the length maths).

### P3 — Non-blocking TCP send (TX buffer)
**Status:** TODO
**Independent:** yes
**Backward compat:** yes
**Risk:** medium

**Where:** `modbus_ble_bridge.cpp`, `send_all_()` (`while` loop with `delay(1)`,
giving up after 1000 ms) called from `handle_modbus_tcp()` and the notify handler.

**Problem:** A stalled TCP client can keep `loop()` busy for up to **1 second**.
ESPHome measures per-component blocking and warns above
`WARN_IF_BLOCKING_OVER_CS = 5` → **50 ms** (`core/component.h:100`,
"took a long time for an operation" in `core/component.cpp:419`), so this path is
20× over the budget and delays BLE event draining and the scheduler for that whole
second.

**Proposed change:** queue the response in a fixed buffer
(`std::array<uint8_t, MAX_TCP_RESPONSE> tx_buf_; size_t tx_len_, tx_sent_;`), have
`send_all_()` only enqueue, and drain a slice per `loop()` iteration; while a
transmission is pending, ignore new requests (as is already done while waiting for
a BLE response). No heap allocation, no YAML change.

**Verification:** compile both configs; measure the blocking warning with a slow
client on hardware (or observe that `loop()` no longer calls `delay()`).

### P2 — Use `parent_->register_for_notify()` where available
**Status:** TODO
**Independent:** yes (related to P1 by design, but appliable alone)
**Backward compat:** yes (`#else` branch keeps the current raw IDF call)
**Risk:** low

**Where:** `modbus_ble_bridge.cpp`, `ESP_GATTC_SEARCH_CMPL_EVT` branch
(`esp_ble_gattc_register_for_notify(...)`).

**Problem:** The component bypasses ESPHome's notification registration helper.
Since 2026.8 `BLEClientBase::register_for_notify()` tracks outstanding requests in
`pending_notify_regs_` and holds the service release until they complete;
`ble_client.h` (2026.8+) documents that a node must "never report Established while
an operation that reads that cache is outstanding". Calling the raw IDF function
means the component sits outside that contract.

**Proposed change:**

```cpp
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 8, 0)
  esp_err_t status = this->parent_->register_for_notify(this->char_read_->handle);
#else
  esp_err_t status = esp_ble_gattc_register_for_notify(this->parent_->get_gattc_if(),
                                                       this->parent_->get_remote_bda(),
                                                       this->char_read_->handle);
#endif
```

`ESPHOME_VERSION_CODE` and `VERSION_CODE()` are available through the generated
`esphome.h` → `core/version.h` → `core/macros.h` chain (verified in the 2026.9.1
build), so no extra include is required.

**Verification:** compile both configs on 2026.7.4 (raw branch) and 2026.8.2/2026.9.1
(new API); functional check of notifications on hardware.

### P1 — Release the GATT cache (report `ESTABLISHED` + write by handle)
**Status:** TODO
**Independent:** yes (see notes) — but is the only item that needs hardware sign-off
**Backward compat:** yes for the YAML; runtime behaviour changes
**Risk:** medium-high

**Where:** `modbus_ble_bridge.cpp` (`ESP_GATTC_SEARCH_CMPL_EVT`,
`ESP_GATTC_NOTIFY_EVT`, `send_ble_request()`), `modbus_ble_bridge.h` (cached
`char_read_` / `char_write_` pointers).

**Problem:** `BLEClientBase::release_services()` deletes every `BLEService`
(and therefore every `BLECharacteristic`) and cleans the stack's GATT cache once the
client and all registered nodes report `espbt::ClientState::ESTABLISHED`. The
component never assigns `node_state` at all (verified: no `node_state` /
`ESTABLISHED` reference in the sources), so it stays in `INIT` and
`all_nodes_established_()` is permanently false — the whole discovered GATT tree
stays in the heap for the lifetime of the connection. (Identical in every supported
version: `USE_ESP32_BLE_DEVICE` is defined and `release_services()` deletes the
services from 2025.9.3 through 2026.9.1.)

The reason it was written this way is real: `BLECharacteristic::write_value()`
dereferences `this->service->client`, so releasing the services while the component
still holds `char_write_` would be a use-after-free.

**Proposed change (two halves, both required):**

1. Stop depending on the `BLECharacteristic` objects: store the discovered
   `uint16_t` handles (`char_read_handle_`, `char_write_handle_`) at
   `ESP_GATTC_SEARCH_CMPL_EVT` and write with the raw IDF call
   `esp_ble_gattc_write_char(parent->get_gattc_if(), parent->get_conn_id(), char_write_handle_, len, data, write_type, ESP_GATT_AUTH_REQ_NONE)`
   (the component already uses raw IDF calls for the notify registration and for the
   notify filter, so this is consistent).
2. Report `node_state = espbt::ClientState::ESTABLISHED` from
   `ESP_GATTC_REG_FOR_NOTIFY_EVT` (i.e. only after the asynchronous registration has
   completed — the ordering ESPHome documents), so the release can run.

Safe on **all** supported versions thanks to that ordering (the raw registration
completes before we establish). P2 adds upstream's outstanding-request accounting on
top on 2026.8+.

**Alternative with zero runtime risk:** gate it behind a YAML option
(`release_ble_gatt_cache: false` by default) so existing devices keep today's
behaviour and the RAM saving is opt-in after measuring.

**Verification:** hardware — connect, run FC3/FC6 (including multiple sequential
requests), force a disconnect/reconnect, and compare free heap before/after. The
13-version compile matrix only proves it builds.

---

## B. Configurability

### P8 — Validate `modbus_port` with `cv.port`
**Status:** TODO
**Independent:** yes
**Backward compat:** yes for valid configs (see note)
**Risk:** very low

**Where:** `components/modbus_ble_bridge/__init__.py` —
`cv.Optional(CONF_MODBUS_PORT, default=502): positive_int`.

**Problem:** `positive_int` is `int_range(min=0)` with **no upper bound**
(`config_validation.py:715`), while the C++ side stores the value in a `uint16_t`
(`set_modbus_port(uint16_t)`). Therefore `modbus_port: 70000` passes validation and
is silently truncated to 4464, and `modbus_port: 0` passes validation and only fails
later in `bind()` with an `ESP_LOGE` at runtime.

**Proposed change:** use `cv.port` (`int_range(min=1, max=65535)`) — available in
**every** supported version (checked in 2025.9.3, 2025.12.7, 2026.5.3 and 2026.9.1).

**Backward compat note:** configurations that are already broken in practice
(`0`, `>65535`) will now fail fast in `esphome config` with a clear message instead
of misbehaving on the device. Ports in range are unaffected.

**Verification:** `esphome config` on both test configs; plus an intentional
`modbus_port: 70000` run to confirm the new error (not part of CI).

### P7 — Expose `ble_response_timeout` in YAML
**Status:** TODO
**Independent:** yes
**Backward compat:** yes (new optional key, default unchanged at 2000 ms)
**Risk:** very low

**Where:** `__init__.py` (new `cv.Optional(CONF_BLE_RESPONSE_TIMEOUT)` + codegen) and
`modbus_ble_bridge.h:26` (`set_ble_response_timeout()`, declared but never called —
`grep` returns only the declaration).

**Problem:** The BLE response timeout is hardcoded to 2000 ms, so tuning it requires
editing C++ and rebuilding. The README already lists it as pending
(`## Roadmap / TODO` → "Expose BLE response timeout as a config option") and the
C++ setter exists, so this is a half-finished feature.

**Proposed change:** add
`cv.Optional(CONF_BLE_RESPONSE_TIMEOUT, default="2s"): cv.positive_time_period_milliseconds`
and `cg.add(var.set_ble_response_timeout(config[CONF_BLE_RESPONSE_TIMEOUT].total_milliseconds))`.
Because the default equals today's value, no existing config changes behaviour.
Document the key (see P13) and/or override it in one test config so CI covers it.

**Verification:** `esphome config` + `compile` on the 13 versions, with the new key
present in at least one test config.

---

## C. CI / regression guards

### P9 — `timeout-minutes` on the CI build job
**Status:** TODO
**Independent:** yes | **Backward compat:** n/a | **Risk:** very low

**Where:** `.github/workflows/ci.yml` (no job defines `timeout-minutes`).

**Problem:** The 13 matrix jobs download toolchains and (for 2026.7+) the native
ESP-IDF framework; a hung download runs until GitHub's default maximum, wasting
runner minutes and hiding the failure as a timeout rather than an error.

**Proposed change:** `timeout-minutes: 45` on the `build` job (and a smaller value on
`report`).

**Verification:** YAML parses; a normal run finishes well inside the limit.

### P10 — Fail CI on any component compiler warning
**Status:** TODO
**Independent:** yes | **Backward compat:** n/a | **Risk:** very low

**Where:** new CI step after "Compile firmware".

**Problem:** Since 2026.7 the native ESP-IDF toolchain compiles ESPHome sources with
`-Wall` (plus `-Werror -Wno-error=…`). The `%u` / `uint32_t` `-Wformat=` findings
were caught by reading a log, not by CI, so a new warning in the component can go
unnoticed across 13 versions — and warnings in this toolchain are one flag-order
change away from being errors.

**Proposed change:** capture the compile output and fail if it contains
`components/modbus_ble_bridge/<file>: warning:` (scoped to our component so that
unrelated ESPHome warnings in any of the 13 versions cannot break the build).

**Verification:** the step passes on the current tree (zero component warnings on
every matrix version, verified in the 2026.7–2026.9 rounds).

### P11 — Assert the conditional `AUTO_LOAD` really skips `sensor`
**Status:** TODO
**Independent:** yes | **Backward compat:** n/a | **Risk:** very low

**Where:** new CI assertion after compiling `tests/test_no_metrics.yaml`.

**Problem:** The component's most subtle behaviour — the callable `AUTO_LOAD` that
skips the `sensor` component when no metrics are configured — is only *documented*,
not tested. If ESPHome changes the auto-load ordering, the "no metrics" binary would
silently start including `sensor` (no compile error, just extra code/RAM).

**Proposed change:** assert on the generated defines: `USE_SENSOR` and
`USE_MODBUS_BLE_BRIDGE_METRICS` absent (and `USE_NETWORK` present) for the
no-metrics build; the opposite for the metrics build. The `defines.h` path is
stable across versions (verified from 2025.9.3 to 2026.9.1).

**Verification:** run the assertion against the existing build dirs for both configs
on 2026.9.1 and confirm the expected 1/0 result reported in the last rounds.

### P12 — Keep the CI matrix and the README version lists in sync
**Status:** TODO
**Independent:** yes | **Backward compat:** n/a | **Risk:** very low

**Where:** new check comparing `.github/workflows/ci.yml` with `README.md`.

**Problem:** The supported versions are maintained by hand in **four** places (the
CI matrix plus three lists in the README). Five rounds of updates produced real
drift: a stale roadmap entry, and a note that reverted to older, incorrect wording.

**Proposed change:** a small Python check (inline CI step or `tools/check_versions.py`)
that parses the matrix with PyYAML and fails if the three README lists do not match
it exactly.

**Verification:** passes on the current tree (13 versions everywhere) and fails if
one list is edited without the others.

---

## D. Documentation

### P13 — Fix the stale roadmap and document every option
**Status:** TODO
**Independent:** yes | **Backward compat:** n/a | **Risk:** none

**Where:** `README.md` — `## Roadmap / TODO`, `## 3. Add the bridge component`, the
metrics table.

**Problem:** The roadmap still lists "Provide Node-RED example flow" although it is
delivered (section 4 links `flows_h1s2_full_v10_mqtt.json`, which is in the repo
root), so the list gives a false picture of what is missing. The options table also
does not mention `ble_response_timeout` once P7 exists.

**Proposed change:** remove/mark the completed roadmap item, add the new optional key
to the options table (and to the example YAML if P7 lands), and — if P1 is
implemented — update the `ESTABLISHED` implementation note accordingly.

**Verification:** documentation only; `P12`'s check keeps the version lists honest.

---

## Deliberately not doing (with rationale)

| Idea | Why not |
| --- | --- |
| Merge the two test YAMLs with `packages:`/anchors | Saves ~20 lines but loses the point of the tests (self-contained, explicit); it would also depend on a syntax that changed in 2026.7 |
| Replace the `(unsigned)` casts with `PRIu32` | Both are correct and portable; the casts are the minimal, already verified fix. Only worth it if we want immunity to future type-width changes (style, not correctness) |
| Raise the component's blocking threshold instead of removing the blocking (P3) | Hides the symptom: the problem is waiting up to 1 s inside `loop()`, not the warning |
| Test the PlatformIO toolchain separately | It is deprecated (removal announced for 2027.2.0) and the native ESP-IDF toolchain has been the default since 2026.7 |

## Verification harness (used by every item)

1. `esphome config tests/test.yaml` and `tests/test_no_metrics.yaml` — exit status 0,
   no warnings.
2. `esphome compile` on both — exit status 0, zero `warning:` / `error:` lines.
3. Generated-artifact checks: `version.h`, `defines.h`
   (`USE_SENSOR`, `USE_MODBUS_BLE_BRIDGE_METRICS`, `USE_NETWORK`,
   `USE_ESP32_BLE_DEVICE`) and `main.cpp` (`HasLoopOverride<…>`,
   `register_ble_node(…)`).
4. Local venvs per version: `.venv-<version>` with the matching `esphome==<version>`.
5. API-drift script: `/tmp/audit/fingerprint.py` (24 checks × all versions).
6. **Keep `tests/.esphome/build/` clean between toolchains**: compiling the same
   config with a different ESPHome release can leave a stale `dependencies.lock`
   in the shared build dir and fail 2025.9.3 with
   `Failed to resolve component 'esp_driver_bitscrambler'` (PlatformIO pins
   IDF 5.4.2 there). Delete `tests/.esphome/build/modbus-ble-bridge-*` before
   switching versions; it is an environment artifact, not a component problem.

**Out of reach here:** anything requiring real hardware (BLE dongle + inverter) —
notably the runtime sign-off for **P1** and the blocking/latency measurement for
**P3**.
