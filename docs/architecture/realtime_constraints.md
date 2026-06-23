# ENDAP V1 Realtime Constraints

## Overview

The ENDAP (Edge-Native Distributed Automation Platform) platform aims to provide deterministic execution for its `control_loop`. However, due to architectural characteristics of the ESP32 platform, there are strict rules and known limitations regarding deterministic execution while handling configuration persistence and Wi-Fi operations.

## Flash Cache Disabling

**"ESP32 SPI Flash erase and NVS persistence operations may temporarily stall non-IRAM code execution for up to approximately 20 ms. This is a platform characteristic and not considered a firmware defect. Such events are expected only during configuration persistence operations."**

Whenever a write or erase operation is performed on the internal SPI flash memory, the ESP-IDF must disable the flash instruction and data caches to prevent stale cache reads and ensure hardware stability.

While the flash cache is disabled:
- The CPU cannot fetch instructions or read constant data from the SPI flash.
- Any task (including high-priority realtime tasks) that attempts to execute code not explicitly placed in IRAM (`IRAM_ATTR`) will be stalled until the flash operation completes.
- NVS operations, especially page erases, can take between 10 ms and 20 ms to complete.

As a result, a non-IRAM control loop can experience a `slip_max` or `exec_max` latency spike of 12 ms to 20 ms if a flash write occurs.

## Realtime Policy

To mitigate these hardware limitations and ensure the highest possible determinism during normal execution:

1. **No Direct Realtime Persistence**: No realtime path or hot-path inside the `control_loop` may call `nvs_commit()` directly.
2. **Auxiliary Execution**: Persistence operations must execute through auxiliary or background services (such as `core_aux_task`), isolated from the main control tasks.
3. **Wi-Fi Persistence Isolation**: The Wi-Fi subsystem is configured to use `WIFI_STORAGE_RAM` (`esp_wifi_set_storage(WIFI_STORAGE_RAM)`). This completely eliminates unpredictable automatic NVS housekeeping operations by the internal Wi-Fi stack during regular connection events.
4. **Deliberate Configuration Saves**: Configuration changes (like saving a Device Profile or updating Wi-Fi credentials) are explicitly triggered events. During these events, a latency spike is acknowledged as a necessary and acceptable side effect of writing to flash.

## Future Mitigations

For ENDAP V1, the system remains non-IRAM to prioritize simplicity, functionality, and memory budget constraints. IRAM optimization is deferred until a future release unless burn-in testing demonstrates a reproducible realtime defect that affects normal operation.

If absolute microsecond determinism is strictly required during concurrent configuration events in the future, all critical control path code, including the `control_loop`, scheduler, timer callbacks, and relevant drivers (e.g., UART for RS485), must be moved into IRAM.
