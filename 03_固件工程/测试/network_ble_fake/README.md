# BLE lifecycle host tests

Compiles the **production** `components/panel_network/src/panel_ble.c` against
small fake FreeRTOS, NimBLE and ESP-Hosted drivers. It drives the public BLE
API and real worker thread; it does not link radio drivers, configure IDF
CMake/Ninja, flash, access COM3 or touch NVS.

## Run

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File '.\03_固件工程\test\network_ble_fake\run_ble_fake.ps1'
```

Expected: `BLE_FAKE_TEST_ALL_PASS scenarios=19`. The runner compiles with
`-Wall -Wextra -Werror` and writes `evidence/run1.log`.

## Scenarios

- `init_retry_after_task_failure`: a queue created before `xTaskCreate` fails
  does not make the next init falsely succeed; the retry creates the worker and
  later calls are idempotent.
- `host_task_exit`: if `nimble_port_run()` unexpectedly returns, the host is
  faulted instead of staying ready.
- `on_off_on`: three normal cycles, one hosted connect / NimBLE init / host task;
  no `nimble_port_stop`, `nimble_port_deinit` or controller disable.
- `scan_cancel`: disable while scanning aborts immediately; late old-session
  scan results/completions cannot revive OFF or pollute a newer scan.
- `scan_cancel_failure`: cancel error becomes a terminal fault; no second init.
- `connect_cancel`: cancel waits for the CONNECT failure event before OFF.
- `connect_cancel_failure`: cancel error becomes a terminal fault.
- `connected_then_disable`: terminate waits for the real DISCONNECT event;
  duplicate late disconnect is ignored; re-enable reuses the host.
- `disconnect_session_guard`: a reused conn_handle with an old connect session
  cannot clear the new connection; the current session disconnect works.
- `teardown_timeout_connect`: no CONNECT-failure event after cancel; re-enable
  does not cancel the old 5 s deadline; timeout faults, the worker still
  consumes commands, and a late event does not revive the host.
- `teardown_timeout_disconnect`: no DISCONNECT after terminate; timeout faults;
  a late DISCONNECT does not become OFF/IDLE.
- `sync_timeout`: 3 s bounded gate, then UNAVAILABLE; late sync cannot clear the
  fault; re-enable does not retry init.
- `host_reset`: reset keeps the initialized marker but enters terminal fault;
  late sync cannot resurrect it; no second init/controller disable.
- `hosted_failure`: `esp_hosted_connect_to_slave` failure is visible and is not
  retried.
- `disable_pending_reenable`: enabling while connect-cancel/terminate is still
  in flight safely reuses the host and settles to IDLE after the event.
- `late_connect_success`: a CONNECT success racing a cancel is terminated.
- `gatt_sessions`: old GATT callbacks are ignored after disconnect/reconnect.
- `addr_failure`: address initialization failure is a terminal fault; late sync
  cannot clear it.
- `modern_init_failure`: >=2.5.2 controller init failure is visible and does not
  fall through to NimBLE init.

## Other checks

Official IDF header syntax (no CMake/Ninja):

```powershell
& 'C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe' `
  '.\03_固件工程\test\network_ble_fake\check_idf_syntax.py'
```

It reuses the exact `build-apps-a2-rev1_3/compile_commands.json` RISC-V command
for `panel_ble.c`, rewrites the stale `C:/wsp` prefix, and runs
`-fsyntax-only`. Expected: `PASS: panel_ble.c with official ESP-IDF 6.0.2 /
esp_hosted 2.13.1 headers`.

BLE status-string glyph coverage against the real raw font cmaps:

```powershell
& 'C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe' `
  '.\03_固件工程\test\network_ble_fake\check_ble_glyphs.py'
```

Expected: `PASS: panel_ble status strings are covered by the existing raw
fonts`. No font is generated or modified.

## Limitations

Host fake evidence is not hardware acceptance. Real C6 HCI sync, scan, connect
and GATT still require Codex to build/flash the P4 and collect UART logs. The
design intentionally keeps one NimBLE host alive across normal BLE off/on; a
terminal fault requires a device restart. If the co-processor never delivers a
CONNECT-failure or DISCONNECT event after cancel/terminate, the snapshot stays
in the honest “正在…” state and never reports a false OFF.
