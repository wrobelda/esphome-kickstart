# Convert an ESP8266 bridge from non-OS V2 to eboot V1

Some ESP8266 vendor firmwares use Espressif's non-OS SDK **V2 user-bin**
layout. A vendor bootloader starts one of two application slots, while the
stock OTA updater writes the other slot. Normal ESPHome firmware uses the
**eboot V1** layout, so moving to ESPHome requires replacing the bootloader as
well as the application.

`esp8266_nonos_v2_to_eboot_v1` lets a temporary Kickstart application boot under
the vendor bootloader and convert its own firmware to the eboot layout. After
conversion, **Kickstart is still running**; ordinary ESPHome OTA can then install
the final device firmware. The component is optional and is not included in
the normal ESP8266 Kickstart image.

```text
Vendor OTA installs a V2 Kickstart application
    ↓
Save a full-flash backup before changing either slot
    ↓
Move Kickstart to the upper V2 slot if necessary
    ↓
Request conversion; Kickstart rebuilds itself for eboot and reboots
    ↓
Install the final device firmware through ordinary ESPHome OTA
```

This document covers the conversion component and the requirements for a
transition profile. Slot operations belong to
[`esp8266_nonos_v2_slot_control`](../esp8266_nonos_v2_slot_control/README.md).
The device's installation guide must supply the hardware values, vendor OTA
procedure, and final firmware configuration.

## Configure a transition profile

The device profile defines the physical flash size, the start and safe capacity
of both V2 application slots, and the virtual address and capacity of
flash-mapped code, called IROM. The component generates a linker script from
these values; it does not infer them from a vendor name or firmware version.

The following fragment illustrates a 2 MiB profile. Use addresses verified
against the target bootloader and stock images. Add the device's Wi-Fi settings
and an authenticated `web_server` with `ota: false` to the complete profile.
This example shares one encryption key between the native API and OTA; the
configuration used for the first firmware upload must use that OTA key too.

```yaml
external_components:
  source: github://wrobelda/esphome-kickstart
  components:
    - hub_api
    - esp8266_nonos_v2_to_eboot_v1
    - esp8266_nonos_v2_slot_control

hub_api:

api:
  encryption:
    key: !secret api_key

ota:
  - platform: esphome
    id: native_ota
    # Use the API encryption key for OTA too.
    encryption:

# Required so safe mode cannot bypass the OTA layout check.
safe_mode:
  disabled: true

esp8266_nonos_v2_to_eboot_v1:
  id: migration
  irom_vma: 0x40201010
  irom_size: 0x0feff0
  flash_size: 0x200000
  slots:
    - offset: 0x001000
      size: 0x100000
    - offset: 0x101000
      size: 0x0fa000
  ota_id: native_ota
  auto_convert: false

esp8266_nonos_v2_slot_control:
  id: slot_control
  auto_copy_lower_to_upper_slot: false
```

These components are supplied by the
[`wrobelda/esphome-kickstart`](https://github.com/wrobelda/esphome-kickstart)
fork of [ESPHome Kickstart](https://github.com/libretiny-eu/esphome-kickstart).

Slot control finds the migration component automatically. Set its optional
`migration_id` only when the profile needs an explicit reference.

Both automatic options default to `false`, so the bridge waits for requests.
This leaves time to download a backup through `hub_api` at
`GET /hub/flash_read`; omitting the range parameters downloads the entire flash,
whose size is device-specific. Save the backup before relocation, because
relocation can overwrite the remaining stock application.

For unattended conversion, enable both `auto_copy_lower_to_upper_slot` and
`auto_convert`. A lower-slot bridge then relocates and reboots, and the
upper-slot bridge converts on the next boot. An upper-slot bridge converts
without relocation. This mode does not wait for a backup request.

## Package the bridge for vendor OTA

The component builds the application but does not package its ELF as a V2
user-bin or implement the vendor's update protocol. Use
[`build_esp8266_nonos_v2.py`](../../tools/build_esp8266_nonos_v2.py) to package
and validate the ELF. The device profile must supply the flash mode, size map,
frequency, entry symbol, IROM mapping, and slot size limit.

The linker script preserves Arduino's `app_entry`, which initializes the
continuation context and UMM heap before entering the non-OS SDK. The resulting
V2 user-bin is the file to offer through vendor OTA. A normal ESPHome
`firmware.bin` is not a substitute for that file.

## Request conversion and check the result

Kickstart must already be running in the upper V2 slot. Use the
[slot-control interface](../esp8266_nonos_v2_slot_control/README.md) to check
the active slot and relocate if needed. The conversion request does not perform
relocation; an HTTP request from the lower slot returns `upper_slot_required`.

| Request | Purpose |
|---|---|
| `POST /hub/convert?confirm=convert-v2-to-eboot` | Record a conversion request and reboot |
| `GET /hub/convert` | Read this boot's `result` and the saved `previous_result`, with their numeric codes |

Protect these routes with the profile's web-server authentication. A successful
POST returns `202 conversion_requested`; this acknowledges the request, not
completion. After the device returns, read the result. `already_converted`
means the bridge recognized the eboot layout on that boot. Check a reported
failure before requesting another attempt.

An automation can use `request_conversion()` instead of HTTP. It requires the
upper V2 slot and refuses further conversion once eboot is running:

```yaml
button:
  - platform: template
    name: "Convert bridge to eboot layout"
    entity_category: diagnostic
    on_press:
      - lambda: id(migration)->request_conversion();
```

The request is saved in RTC-backed preferences and consumed on the next boot.
Conversion runs before Wi-Fi starts, rather than inside the HTTP callback.
A power loss can discard the request; reconnect and inspect the result before
retrying. The component does not check whether the caller has saved a backup.

## How conversion works

The V2 user-bin and V1 image contain the same compiled application in different
formats. Kickstart reconstructs its V1 image from the running V2 copy, so
conversion needs neither recompilation nor an uploaded replacement image.

The component:

1. Checks the physical flash size, requires the upper slot, and validates the
   source image and destination bounds.
2. Rebuilds the V1 application headers and segments using the linked IROM
   length and the V2 image's RAM segments.
3. Writes the application from `0x1000` upward, while holding eboot in RAM.
4. Patches the whole-image size and CRC and verifies the written application.
5. Writes and verifies eboot at address zero, then reboots.

On the eboot boot, Kickstart invalidates the stale upper-slot V2 header. Future
firmware installations use ordinary ESPHome OTA.

The bootloader bytes are stored in [`eboot_v1_data.py`](eboot_v1_data.py).
[`generate_eboot_v1.py`](../../tools/generate_eboot_v1.py) creates that module
from the first 4096 bytes of an ESP8266 factory image. ESPHome code generation
embeds the stored bytes; it does not automatically extract a new bootloader
from the installed framework. Keep the embedded bootloader compatible with
the framework used to build the bridge.

[`esp8266_self_convert.py`](../../tools/esp8266_self_convert.py) provides a
host-side reconstruction tool. Its fixture tests complement the device
implementation; they do not replace a target build and hardware migration test.

## Ordinary OTA and Device Builder

The standard ESP8266 OTA backend assumes eboot is running. Set `ota_id` to the
native OTA instance so the transition component can prevent that instance from
starting while the bridge still uses V2. After conversion and reboot, the same
configuration starts native OTA normally. Keep the OTA credentials compatible
with the final configuration used by the installer.

ESPHome safe mode can start OTA without starting the transition component.
The profile must therefore set `safe_mode: disabled: true`; configuration
validation rejects profiles that leave it enabled.

Do not enable `captive_portal`: its separate OTA upload remains available even
with `web_server: ota: false`, and `ota_id` does not gate that path. This
behavior is documented in [ESPHome PR #9583](https://github.com/esphome/esphome/pull/9583).
Configure station credentials in the build instead. A password-protected
`wifi: ap:` fallback still allows access to the recovery web interface without
the captive portal.

`dashboard_import` can advertise the final device configuration while Kickstart
is still V2. Take Control in ESPHome Device Builder creates a configuration; it
does not install firmware. Install only after conversion has completed, using a
package that resolves its components and credentials outside the original
checkout.

Keeping the same native API encryption key avoids re-authentication in Home
Assistant. If the final configuration uses a different key, Home Assistant
must obtain that key before reconnecting. API credentials and OTA credentials
are separate unless the profile explicitly shares them, as in the example.

## Recovery limits

Before relocation or conversion, the other slot may still contain the stock
application. The [slot-control interface](../esp8266_nonos_v2_slot_control/README.md)
can validate and boot that slot. A full-flash download at this stage is still
post-vendor-OTA: one stock application has already been replaced by Kickstart.

If conversion fails before writing eboot, the lower application area may have
changed, but the vendor bootloader and upper bridge remain available for
recovery. Writing eboot last limits the time spent changing the boot layout;
it does not make that sector update atomic. Power loss during its erase or
write can require serial recovery.

This component does not convert eboot back to the vendor layout. After eboot
replacement, use the device's serial recovery procedure and a suitable saved
image to restore stock firmware.

The application-first, bootloader-last sequence also appears in
[SonOTA's Espressif2Arduino bridge](https://github.com/mirko/SonOTA). This
component is an independent implementation.
