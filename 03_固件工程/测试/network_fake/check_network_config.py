import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
if path.suffix.lower() == ".json":
    cfg = json.loads(path.read_text(encoding="utf-8"))
else:
    cfg = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        s = raw.strip()
        if not s:
            continue
        if s.startswith("# ") and s.endswith(" is not set"):
            key = s[2:-11].strip()
            if key.startswith("CONFIG_"):
                key = key[7:]
            cfg[key] = False
            continue
        if s.startswith("#"):
            continue
        if "=" not in s:
            continue
        key, value = s.split("=", 1)
        if key.startswith("CONFIG_"):
            key = key[7:]
        value = value.strip()
        if value in ("y", "n"):
            cfg[key] = value == "y"
        elif value.startswith('"') and value.endswith('"'):
            cfg[key] = value[1:-1]
        else:
            try:
                cfg[key] = int(value)
            except ValueError:
                cfg[key] = value

want = {
    "ESP_HOSTED_CP_TARGET_ESP32C6": True,
    "ESP_HOSTED_CP_TARGET_ESP32H2": False,
    "ESP_HOSTED_IDF_SLAVE_TARGET": "esp32c6",
    "ESP_HOSTED_SDIO_HOST_INTERFACE": True,
    "ESP_HOSTED_SPI_HOST_INTERFACE": False,
    "ESP_HOSTED_SDIO_SLOT_1": True,
    "ESP_HOSTED_SDIO_SLOT_0": False,
    "ESP_HOSTED_SDIO_4_BIT_BUS": True,
    "ESP_HOSTED_SDIO_1_BIT_BUS": False,
    "ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1": 19,
    "ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1": 18,
    "ESP_HOSTED_PRIV_SDIO_PIN_D0_SLOT_1": 14,
    "ESP_HOSTED_PRIV_SDIO_PIN_D1_4BIT_BUS_SLOT_1": 15,
    "ESP_HOSTED_PRIV_SDIO_PIN_D2_4BIT_BUS_SLOT_1": 16,
    "ESP_HOSTED_PRIV_SDIO_PIN_D3_4BIT_BUS_SLOT_1": 17,
    "ESP_HOSTED_SDIO_GPIO_RESET_SLAVE": 54,
    "BT_ENABLED": True,
    "BT_CONTROLLER_DISABLED": True,
    "BT_BLUEDROID_ENABLED": False,
    "BT_NIMBLE_ENABLED": True,
    "BT_NIMBLE_TRANSPORT_UART": False,
    "ESP_HOSTED_ENABLE_BT_NIMBLE": True,
    "ESP_HOSTED_NIMBLE_HCI_VHCI": True,
    "ESP_HOSTED_MEMPOOL_PREFER_SPIRAM": True,
}
failed = 0
for key, expected in want.items():
    got = cfg.get(key)
    if got != expected:
        print(f"FAIL {key}: got {got!r}, want {expected!r}")
        failed += 1
for key, expected in (("ESP_HOSTED_SDIO_PIN_CMD", 19), ("ESP_HOSTED_SDIO_PIN_CLK", 18)):
    got = cfg.get(key)
    if got != expected:
        print(f"FAIL {key}: got {got!r}, want {expected!r}")
        failed += 1
print("NETWORK_CONFIG_OK" if not failed else f"NETWORK_CONFIG_FAIL failures={failed}")
sys.exit(1 if failed else 0)
