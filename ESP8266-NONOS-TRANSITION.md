# ESP8266 non-OS V2 to eboot V1 transition

Some ESP8266 vendor firmwares use Espressif's paired non-OS SDK **V2 user-bin**
layout: a vendor bootloader starts one of two application slots, while OTA
writes the other slot. Normal ESPHome firmware uses the **eboot V1** layout.
Moving between these layouts requires replacing the bootloader as well as the
application.

The `esp8266_nonos_v2_to_eboot_v1` component builds a temporary ESPHome
application that can run under the vendor bootloader and install the final
eboot V1 factory image. It is optional and is not enabled in the normal
ESP8266 Kickstart image.

```text
Vendor OTA installs a V2 Kickstart application
    ↓
Kickstart runs in the upper V2 slot, relocating there if needed
    ↓
An operator or installer uploads the final ESPHome factory image
    ↓
Kickstart writes the application, validates it, and writes eboot last
    ↓
The final firmware boots in the eboot V1 layout
```

The vendor-specific installation procedure must supply the correct image and a
backup and recovery path. This guide covers the generic layout components and
their authenticated HTTP interfaces.

## Configure the transition profile

The migration component owns all layout metadata. A device profile supplies:

- the physical flash size;
- the start and safe capacity of each V2 application slot;
- the virtual address and capacity of flash-mapped code, called IROM.

The component generates a linker script from those values. The linker script
preserves Arduino's `app_entry`, which initializes the continuation context and
UMM heap before entering the non-OS SDK. The component does not infer addresses
from a vendor name or firmware version.

The following fragment illustrates the schema. Use values verified from the
target bootloader and stock images, and include an authenticated `web_server`
in the complete configuration. The transition components are supplied by the
[`wrobelda/esphome-kickstart`](https://github.com/wrobelda/esphome-kickstart)
fork of [ESPHome Kickstart](https://github.com/libretiny-eu/esphome-kickstart).

```yaml
external_components:
  source: github://wrobelda/esphome-kickstart
  components:
    - hub_api
    - esp8266_nonos_v2_to_eboot_v1
    - esp8266_nonos_v2_slot_control

hub_api:

ota: false

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

`hub_api` provides full-flash downloads. The optional
`esp8266_nonos_v2_slot_control` component provides slot selection and relocation,
using the layout already owned by the migration component. Omitting slot
control also omits its HTTP routes.

## Package the bridge for vendor OTA

The migration component builds the application but does not package its ELF as
a V2 user-bin or implement the vendor's update protocol.

Use [`tools/build_esp8266_nonos_v2.py`](tools/build_esp8266_nonos_v2.py) to package
and validate the ELF. The vendor installation profile must supply the flash
mode, size map, frequency, entry symbol, IROM mapping, and slot size limit.
The resulting V2 file is the input to vendor OTA; the final ESPHome factory
image is the input to Kickstart's migration endpoint.

## Move to the upper slot

A normal ESPHome factory image begins at flash address zero and extends through
the lower application area. Kickstart must therefore run from the upper V2
slot before installing that image, so the installer does not overwrite its
running code.

With `auto_copy_lower_to_upper_slot: true`, a lower-slot bridge relocates during
hardware setup, before ESPHome initializes Wi-Fi and lwIP networking. It then
reboots into the upper copy. A bridge already running in the upper slot needs
no relocation.

The copy proceeds as follows:

1. Validate the running lower-slot V2 image.
2. Erase the upper image's header sector, leaving the destination invalid
   during the copy.
3. Copy the image, writing the destination header sector last.
4. Validate the upper copy, select that slot, and reboot.

The lower image remains intact during copying. If the copy is interrupted,
the incomplete upper image must not be selected. Relocation can overwrite the
remaining vendor application, so a later full-flash download is not necessarily
a stock-firmware backup.

For manual relocation, omit `auto_copy_lower_to_upper_slot` and use the copy
route below. That request returns before flash work starts; validation and
copying run in the normal ESPHome component context rather than in the
ESPAsyncWebServer callback.

### Slot-control routes

All routes require the configured web-server authentication.

| Request | Purpose |
|---|---|
| `GET /hub/slot_status` | Report the active slot, flash size, and relocation status |
| `POST /hub/boot_other?confirm=boot-other` | Validate the other V2 image, select it, and reboot |
| `POST /hub/copy_lower_to_upper_slot?confirm=copy-lower-to-upper-slot` | Copy a valid running lower-slot bridge to the upper slot, validate the copy, and boot it |

## Install the final ESPHome image

The migration component exposes authenticated `GET` and `POST` requests at
`/hub/migrate`. An operator or installation tool must supply the complete
ESP8266 `firmware.factory.bin`; an OTA application image alone is insufficient.
Kickstart does not start the final-image upload on its own.

The installer performs these steps:

1. Check the physical flash size and require execution from the upper V2 slot.
2. Hold the new eboot sector in RAM while writing the application from flash
   address `0x1000` upward. Reject writes that would reach the running slot.
3. Validate the bootloader and application E9 images, segment ranges and
   overlap, segment checksums, Arduino's whole-image size and CRC, and flash
   readback.
4. Write the eboot sector at address zero last, then reboot into the eboot V1
   layout.

Application data is written during upload; full-image validation finishes
before the bootloader is replaced. Validation failure therefore does not imply
that the lower application area is unchanged.

### Upload from the command line

This command prompts for the configured web-server password. Replace
`WEB_USERNAME` and `KICKSTART_ADDRESS` with the device's settings:

```sh
curl --digest --user WEB_USERNAME --fail-with-body \
  --form firmware=@firmware.factory.bin \
  'http://KICKSTART_ADDRESS/hub/migrate?confirm=replace-vendor-bootloader'
```

### Ordinary OTA and Home Assistant

Set `ota: false` in the transition configuration. Configuration validation
rejects ESPHome's standard OTA component because its backend assumes the eboot
V1 layout is already active. The migration component also rejects `/update`,
which the captive portal can expose even when `web_server.ota` is false. The
captive portal still supports Wi-Fi provisioning.

Enable standard ESPHome OTA in the final eboot V1 configuration. Use the same
native API encryption key in the transition and final configurations so Home
Assistant can reuse the existing device entry, even if the node and friendly
names change.

### Power-loss limits

Writing the bootloader last leaves the vendor bootloader and running upper
application intact throughout the longer application write and validation
stages. It cannot make the final sector update atomic: a power loss during the
erase or write of sector zero can still require serial recovery.

The transition uses the application-first, bootloader-last pattern also used
by [SonOTA's Espressif2Arduino bridge](https://github.com/mirko/SonOTA). This
component is an independent implementation.

## Relationship to the normal Kickstart handoff

The source and destination layouts determine the update path:

| Running layout | Incoming layout | Update path |
|---|---|---|
| eboot V1 | eboot V1 | Normal ESPHome OTA |
| non-OS V2 | non-OS V2 | Vendor-compatible V2 packaging and OTA |
| non-OS V2 | eboot V1 | This migration component with a complete factory image |

Standard Kickstart images enable ESPHome OTA and `dashboard_import`. Their
layout is compatible with the final ESPHome image, so Device Builder can import
a configuration and install it through ordinary OTA.

A non-OS V2 bridge instead requires `/hub/migrate` for the final installation.
Transparent migration through a normal ESPHome OTA request is not implemented.
The transition component also does not provide an eboot V1-to-non-OS V2
restoration path; restoring vendor firmware requires the device's serial
recovery procedure.
