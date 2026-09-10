# Manage ESP8266 non-OS V2 application slots

`esp8266_nonos_v2_slot_control` inspects and switches the two application slots
used by an ESP8266 vendor bootloader. It can also copy a running lower-slot
Kickstart application to the upper slot. These operations preserve the V2
layout; they do not replace the bootloader.

The component uses the layout configured in
[`esp8266_nonos_v2_to_eboot_v1`](../esp8266_nonos_v2_to_eboot_v1/README.md).
Read that component's guide for profile setup, backup timing, and conversion
to eboot. This document covers only operations within the V2 layout.

## Configure slot control

Add this fragment to a transition profile with an authenticated `web_server`
and an `esp8266_nonos_v2_to_eboot_v1` component:

```yaml
esp8266_nonos_v2_slot_control:
  id: slot_control
  # Optional: resolved automatically when only one migration component exists.
  migration_id: migration
  auto_copy_lower_to_upper_slot: false
```

Automatic copying is off by default. Leave it off when a caller needs to save
a backup before changing either slot.

## Inspect the slots

`GET /hub/slot_status` reports the flash size, active slot, relocation status,
and validity and entry point of both images. Slot 1 is the lower slot; slot 2
is the upper slot. The `current_*` fields describe the running slot, while
`other_*` describes the inactive slot.

A valid image is not necessarily stock firmware: the V2 format has no
structured vendor or version field. The status reports image validity and
entry points, not firmware identity. These slot fields describe the vendor
layout and should not be used to identify the running application after eboot
conversion.

The component scans the images from the main loop after setup and caches the
result, so reads do not perform a flash scan inside an HTTP callback. The YAML
getters expose the same cached information. Because the slot fields describe
the vendor layout, gate them on the migration component's `layout_is_eboot()`
so one diagnostic reports the layout and, only under V2, the slot detail:

```yaml
text_sensor:
  - platform: template
    name: "Layout type"
    update_interval: 5min
    lambda: |-
      char buf[160];
      if (id(migration)->layout_is_eboot()) {
        snprintf(buf, sizeof(buf), "eboot V1");
      } else {
        snprintf(buf, sizeof(buf),
                 "V2, slot %u (%s) entry 0x%08X, other slot %s entry 0x%08X",
                 id(slot_control)->active_slot_number(),
                 id(slot_control)->active_slot_valid() ? "valid" : "invalid",
                 id(slot_control)->active_slot_entry(),
                 id(slot_control)->inactive_slot_valid() ? "valid" : "invalid",
                 id(slot_control)->inactive_slot_entry());
      }
      return { buf };
```

## Switch or relocate

Protect all routes with the profile's web-server authentication. Use these
operations only while the vendor bootloader is running.

| Request | Effect |
|---|---|
| `POST /hub/boot_other?confirm=boot-other` | Validate the other V2 image, select that slot, and reboot |
| `POST /hub/copy_lower_to_upper_slot?confirm=copy-lower-to-upper-slot` | Copy the running lower-slot bridge to the upper slot, validate the copy, and boot it |

`request_boot_other()` and `request_copy_lower_to_upper()` expose the same
operations to a template button or automation. For example:

```yaml
button:
  - platform: template
    name: "Switch to other slot"
    entity_category: diagnostic
    on_press:
      - lambda: id(slot_control)->request_boot_other();
  - platform: template
    name: "Relocate to upper slot"
    entity_category: diagnostic
    on_press:
      - lambda: id(slot_control)->request_copy_lower_to_upper();
```

Switching slots does not copy an image. If the other slot still holds a valid
stock application, switching provides a way back to that application before
the bootloader is replaced.

Relocation is different: **it overwrites the upper application slot**. When
vendor OTA installed Kickstart into the lower slot, the upper slot contains
the remaining stock application. After relocation, both slots contain
Kickstart; a later backup can no longer preserve that stock application.

## How relocation works

The bridge needs the upper slot when another operation will overwrite the
lower application area. Relocation proceeds as follows:

1. Validate the running lower-slot V2 image.
2. Erase the upper image's header sector, leaving the destination invalid.
3. Copy the image, writing the destination header sector last.
4. Validate the copy, select the upper slot, and reboot.

The lower image remains intact during copying. An explicit copy request
returns before the flash work starts; copying runs in ESPHome's component
context, not in the web-server callback. Wait for the reboot and inspect
`/hub/slot_status` before proceeding.

With `auto_copy_lower_to_upper_slot: true`, the same relocation starts during
hardware setup, before Wi-Fi, whenever the bridge starts in the lower slot.
A bridge already in the upper slot does not need to move. This option controls
relocation only; it does not request eboot conversion.
