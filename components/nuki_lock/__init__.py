"""Build requirements shared by everything that uses the Nuki Bluetooth code in this directory:
the `nuki_lock` lock platform (lock.py) and the `nuki_hub` component.
"""
from pathlib import Path

import esphome.codegen as cg
from esphome.components.esp32 import add_idf_component, add_idf_sdkconfig_option, arduino_bundles_libsodium
from esphome.components.noise import LIBSODIUM_VERSION
from esphome.core import CORE

# ESP-IDF component compiling the libsodium modules ESPHome's port leaves out
NUKI_SODIUM_DIR = Path(__file__).resolve().parent / "nuki_sodium"

_KEY_BUILD_DONE = "nuki_lock_build_requirements"


def add_nuki_build_requirements():
    """Adds the libraries, sdkconfig options and build flags once per build."""
    if CORE.data.get(_KEY_BUILD_DONE):
        return
    CORE.data[_KEY_BUILD_DONE] = True

    if arduino_bundles_libsodium():
        # arduino-esp32 (below IDF 6.0) ships espressif/libsodium, and ESPHome's
        # noise component then falls back to its PlatformIO libsodium library.
        # Ignore that one so espressif's full libsodium is the only copy;
        # noise-c links against it via the shared managed-components requirement.
        cg.add_platformio_option("lib_ignore", ["libsodium"])
        add_idf_component(
            name="espressif/libsodium",
            ref="^1.0.20~2",
        )
        # libsodium's HMAC-SHA256 can return wrong results when backed by hardware SHA
        # acceleration via mbedTLS (likely related to esphome/esphome#12707, #13021, #13234).
        # Force libsodium to use its own SHA implementation instead.
        add_idf_sdkconfig_option("CONFIG_LIBSODIUM_USE_MBEDTLS_SHA", False)
    else:
        # Everywhere else ESPHome's noise component (`api` encryption) adds its own
        # esphome/libsodium as an ESP-IDF managed component, which can't coexist with
        # espressif/libsodium. Always use ESPHome's port (same version as noise, so
        # both requests dedupe) and compile the modules it leaves out in nuki_sodium.
        add_idf_component(
            name="esphome/libsodium",
            ref=LIBSODIUM_VERSION,
        )
        add_idf_component(
            name="nuki_sodium",
            path=str(NUKI_SODIUM_DIR),
        )

    add_idf_component(
        name="crc16",
        repo="https://github.com/AzonInc/Crc16.git",
    )
    # NukiBleEsp32 (lock-only subset, Opener files excluded) is vendored directly as
    # component sources instead of a fetched IDF component, see nuki_ble.cpp/.h,
    # nuki_lock_protocol.cpp/.h etc. in this directory. It now talks to the lock via
    # ESPHome's own (Bluedroid-based) BLE stack (esp32_ble_tracker/esp32_ble_client)
    # instead of NimBLE.

    # Nuki relies on seeing every advertisement to catch lock-state-changed notifications
    # (a status bit inside the manufacturer data - see NukiBle::parse_device()), so
    # duplicate-advertisement filtering must stay off.
    add_idf_sdkconfig_option("CONFIG_BTDM_BLE_SCAN_DUPL", False)

    # Keep the lock's GATT services cached in NVS (like bluetooth_proxy's cache_services).
    # Without it, ESPHome clears Bluedroid's cache after every disconnect and each
    # connection runs a full service discovery (~5 s with Nuki locks) before a command
    # can be sent. A cache that no longer fits the lock is dropped by NukiBle.
    add_idf_sdkconfig_option("CONFIG_BT_GATTC_CACHE_NVS_FLASH", True)

    # Build flags
    cg.add_build_flag("-Wno-unused-result")
    cg.add_build_flag("-Wno-ignored-qualifiers")
    cg.add_build_flag("-Wno-missing-field-initializers")
    cg.add_build_flag("-Wno-maybe-uninitialized")
