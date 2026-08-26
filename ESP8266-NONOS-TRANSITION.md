# ESP8266 non-OS SDK transition profiles

`kickstart_transition` builds an ESPHome application for a vendor bootloader
that uses Espressif's paired non-OS SDK user-bin layout. It is not enabled by
the normal ESP8266 Kickstart image.

The device profile supplies the physical flash size, both slot ranges, and the
virtual address and capacity of flash-mapped code. The component generates the
linker script from those values. It does not assume a particular vendor or
firmware version.

```yaml
external_components:
  source: github://libretiny-eu/esphome-kickstart
  components:
    - hub_api
    - kickstart_transition
    - kickstart_slot_control

hub_api:

kickstart_transition:
  irom_vma: 0x40200000
  irom_size: 0x100000

kickstart_slot_control:
  flash_size: 0x200000
  slots:
    - offset: 0x001000
      size: 0x100000
    - offset: 0x101000
      size: 0x0fb000
```

The example values demonstrate the schema; use values verified from the target
bootloader and stock images. `kickstart_slot_control` is optional. If it is
omitted, no slot-status or boot-switch route is compiled. If it is present,
authenticated `POST /hub/boot_other` with `confirm=boot-other` asks the
Espressif SDK to select the other slot after checking its V2 magic.

The component does not package the ELF as a V2 image and does not implement a
vendor's update protocol. Those operations belong to a separate image builder
and vendor installation profile.
