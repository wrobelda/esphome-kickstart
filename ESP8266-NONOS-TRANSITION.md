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
  auto_copy_lower_to_upper_slot: true
```

The example values demonstrate the schema; use values verified from the target
bootloader and stock images. The migration component owns the layout, so
other components do not repeat it.

`esp8266_nonos_v2_slot_control` is optional. If it is omitted, the slot-management
routes are not compiled. The example enables automatic lower-to-upper relocation;
omit `auto_copy_lower_to_upper_slot` when relocation must be started through the
HTTP route instead. If the component is present, it adds these authenticated routes:

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

Relocation must run before ESPHome initializes networking. An earlier
implementation scheduled it after normal setup; the tested lower-slot image
then raised `LoadProhibit` while ESPHome reconfigured Wi-Fi and lwIP state
inherited from the vendor firmware. The working implementation relocates at
hardware setup priority and reboots before Wi-Fi setup.

## Install the final ESPHome image

`esp8266_nonos_v2_to_eboot_v1` exposes authenticated `GET` and `POST` requests at
`/hub/migrate`. Upload the complete ESP8266 `firmware.factory.bin`, not an OTA
application image. The component allows an operator or installation tool to
start migration through this endpoint; the component does not initiate the
final-image upload on its own. The component:

1. Refuses installation unless it is executing from the upper V2 slot.
2. Keeps the new eboot sector in RAM while writing the application from flash
   address `0x1000` upward.
3. Validates both E9 images, segment destination ranges and overlap, segment
   checksums, Arduino's whole-image size and CRC, and flash readback.
4. Writes the eboot sector at address zero last, then reboots into the normal
   ESPHome layout.

Set `ota: false` in the transition configuration. Configuration validation
rejects ESPHome's standard OTA component because that OTA backend assumes the
eboot V1 layout is already active. Enable standard ESPHome OTA in the final
eboot V1 configuration instead.

Use the same native API encryption key in the transition and final
configurations. Home Assistant can then reuse the existing device-registry
entry even when the final configuration changes the node and friendly names.

The migration endpoint can also be driven without a browser. This command
prompts for the configured web-server password:

```sh
curl --digest --user WEB_USERNAME --fail-with-body \
  --form firmware=@firmware.factory.bin \
  'http://KICKSTART_ADDRESS/hub/migrate?confirm=replace-vendor-bootloader'
```

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

## Relationship to the normal Kickstart handoff

The source and destination image layouts determine the update path:

| Running layout | Incoming layout | Update path |
|---|---|---|
| eboot V1 | eboot V1 | normal ESPHome OTA |
| non-OS V2 | non-OS V2 | vendor-compatible V2 packaging and OTA |
| non-OS V2 | eboot V1 | this migration component with a complete factory image |
| eboot V1 | non-OS V2 | not implemented; requires a separate reverse-migration design |

The standard Kickstart images enable ESPHome OTA and `dashboard_import`. Their
running layout is compatible with the final ESPHome image, so Device Builder
can import the selected configuration and install it through ordinary ESPHome
OTA.

This transition component handles a different case: the running vendor V2
layout is incompatible with the final eboot V1 image. Its current dedicated
endpoint keeps the unsafe ordinary OTA backend unavailable. A future
Kickstart-provided OTA backend should accept the normal authenticated ESPHome
OTA request, detect the running and incoming layouts, and perform this
migration internally; users should not need to select a special route.
Implement and demonstrate that handoff in Kickstart first. Whether ESPHome
core should later absorb the migration backend, or the wider Kickstart
project, is a separate design decision for both projects.
