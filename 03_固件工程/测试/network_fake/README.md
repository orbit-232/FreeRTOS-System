# Network host fake / config gates

These tests are build-time-independent host evidence. They never link radio
drivers and never touch COM3.

## Production worker/event fake

This compiles the **production** `panel_network.c` with the small fake IDF/RTOs
under `include/` and `src/`; it exercises the actual worker task, event queue,
NVS blob, generation/session checks, timeout budget and connect/disconnect
ordering.

```powershell
$root = 'C:\wsp\WavesharePanel\03_Firmware'
gcc -std=gnu11 -O0 -g -w -DESP_PLATFORM `
  -I "$root\test\network_fake\include" `
  -I "$root\components\panel_network\include" `
  "$root\test\network_fake\test_network_fake.c" `
  "$root\components\panel_network\src\panel_network.c" `
  "$root\components\panel_network\src\panel_network_model.c" `
  "$root\test\network_fake\src\fake_rtos.c" `
  "$root\test\network_fake\src\fake_wifi.c" `
  "$root\test\network_fake\src\fake_nvs.c" `
  -o "$root\test\network_fake\test_network_fake.exe"
& "$root\test\network_fake\test_network_fake.exe"
```

Expected: `NETWORK_FAKE_TEST failures=0`.

The fake explicitly emits disconnect/GOT-IP/driver events. It covers
enable→off→late event, open AP, remember=false, remember=true, wrong AP,
late/mismatched GOT_IP, overall 15 s budget, scan while connected and late scan
completion.

## Pure model

```powershell
gcc -std=c99 -Wall -Wextra -I "$root\components\panel_network\include" `
  "$root\test\network_model\test_network_model.c" `
  "$root\components\panel_network\src\panel_network_model.c" `
  -o "$root\test\network_model\test_network_model.exe"
& "$root\test\network_model\test_network_model.exe"
```

Expected: `NETWORK_MODEL_TEST failures=0`.

## Final generated SDK config

Codex must run this after its independent configure/build, against
`build-<name>/config/sdkconfig.json`:

```powershell
python "$root\test\network_fake\check_network_config.py" `
  "$root\build-apps-a3-rev1_3\config\sdkconfig.json"
```

Expected: `NETWORK_CONFIG_OK`. It checks C6 target, SDIO (not SPI), slot 1,
CMD19/CLK18/D0 14/D1 15/D2 16/D3 17/reset54, 4-bit/25 MHz, and NimBLE VHCI.

BLE source was compiled with the downloaded IDF 6.0 NimBLE + esp_hosted 2.13.1
headers using `-fsyntax-only`; BLE connection behavior is not hardware-verified
until Codex builds/flashes and runs the UART `bleon/blescan/blestop/bleoff`
commands.
