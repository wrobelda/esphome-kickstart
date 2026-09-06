# ESP8266 non-OS SDK transition profiles

`esp8266_nonos_v2_to_eboot_v1` builds an ESPHome application for a vendor bootloader
that uses Espressif's paired non-OS SDK user-bin layout. It is not enabled by
the normal ESP8266 Kickstart image.

The device profile supplies the physical flash size, both slot ranges, and the
virtual address and capacity of flash-mapped code. The component generates the
linker script from those values. It does not assume a particular vendor or
firmware version. The script preserves Arduino's `app_entry`, which initializes
the continuation context and UMM heap before entering the non-OS SDK.

```yaml
external_components:
  source: github://libretiny-eu/esphome-kickstart
  components:
    - hub_api
    - esp8266_nonos_v2_to_eboot_v1
    - esp8266_nonos_v2_slot_control

hub_api:

esp8266_nonos_v2_to_eboot_v1:
  irom_vma: 0x40201010
  irom_size: 0x0feff0
  flash_size: 0x200000
  slots:
    - offset: 0x001000
      size: 0x100000
    - offset: 0x101000
      size: 0x0fa000

esp8266_nonos_v2_slot_control:
```

The example values demonstrate the schema; use values verified from the target
bootloader and stock images. The migration component owns the layout, so
other components do not repeat it.

`esp8266_nonos_v2_slot_control` is optional. If it is omitted, the slot-management
routes are not compiled. If it is present, it adds these authenticated routes:

- `GET /hub/slot_status` reports the active slot, physical flash size, and
  lower-to-upper relocation status.
- `POST /hub/boot_other?confirm=boot-other` validates and boots the
  other V2 image.
- `POST /hub/copy_lower_to_upper_slot?confirm=copy-lower-to-upper-slot` copies
  a valid running lower-slot transition image into the upper slot, validates
  the copy, and boots it. The destination header sector is erased first and
  written last, so an interrupted copy leaves the running lower image intact
  and the incomplete upper image invalid. The request returns before flash
  work begins, so validation and copying run from the normal ESPHome component
  context rather than the ESPAsyncWebServer callback.

The upper-slot relocation is needed when the vendor OTA mechanism installs the
first transition image in the lower slot. A normal ESPHome factory image starts
at flash address zero and extends through the lower application area, so it
must be installed while the transition application executes from the upper
slot.

## Install the final ESPHome image

`esp8266_nonos_v2_to_eboot_v1` exposes authenticated `GET` and `POST` requests at
`/hub/migrate`. Upload the complete ESP8266 `firmware.factory.bin`, not an OTA
application image. The component:

1. Refuses installation unless it is executing from the upper V2 slot.
2. Keeps the new eboot sector in RAM while writing the application from flash
   address `0x1000` upward.
3. Validates both E9 images, segment destination ranges and overlap, segment
   checksums, Arduino's whole-image size and CRC, and flash readback.
4. Writes the eboot sector at address zero last, then reboots into the normal
   ESPHome layout.

Use the same native API encryption key in the transition and final
configurations. Home Assistant can then reuse the existing device-registry
entry even when the final configuration changes the node and friendly names.

The component also intercepts `/update` and rejects normal ESPHome web OTA.
ESPHome's captive portal otherwise enables that route even when
`web_server.ota` is false, but its OTA backend assumes that eboot already owns
the flash layout. Wi-Fi provisioning through the captive portal remains
available.

Writing the bootloader last is the same transition pattern used by SonOTA's
Espressif2Arduino bridge. This implementation is independent because that old
sketch is device-specific and has no license that permits copying it as a
library.

A power loss during the final erase or write of flash sector zero can still
require serial recovery. Writing the bootloader last protects the much longer
application write and validation stages; it cannot make a flash-sector update
atomic.

The component does not package its own ELF as a V2 image and does not implement
a vendor's update protocol. Those operations belong to a separate image
builder and vendor installation profile. `tools/build_esp8266_nonos_v2.py`
packages and validates the component's ELF; the vendor profile supplies its
flash mode, size map, frequency, entry symbol, IROM mapping, and slot limit.
