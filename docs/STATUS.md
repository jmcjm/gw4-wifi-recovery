# Verified state

Device: Samsung Galaxy Watch 4 (SM-R870), firmware `R870XXS2JZG3` (Android 16, One UI 8 Watch), bootloader unlocked, root through Magisk 31.1.

## Recovery partition

| Item | Value |
|---|---|
| Partition | `/dev/block/by-name/recovery` → `mmcblk0p19` |
| Image size | 55 574 528 bytes |
| Boot image header | version 2, kernel gzip, ramdisk `lz4_legacy`, with `dtb` and `recovery_dtbo` |
| Kernel command line | `bootconfig buildtime_bootconfig=enable loop.max_part=7` |
| Stock recovery SHA-256 | `f33cfb9bc6fc5909b14d0a5ef4ddc2edab8797b5341b0d207a450e874b69a809` |
| Built image SHA-256 (as tested) | `be79349724fad783e82697b4fa67199f94fba2e50ded1a1b0a3ed474263cb0e6` |

`build.sh` reproduced the tested image byte for byte from the stock image, the patched policy and the same access point password. A different password gives a different hash.

## Hardware seen from recovery

| Item | Value |
|---|---|
| Display | `/dev/graphics/fb0`, fbdev, 450×450, 32 bpp, virtual 450×900, stride 1800; no `/dev/dri` |
| Backlight | `/sys/class/backlight/panel` |
| Upper key | `s2mpw03-power-keys`, `KEY_POWER` |
| Lower key | `gpio_keys`, `KEY_APPSELECT` |
| Wrist motion | `LPM_MOTION`, `KEY_HOMEPAGE` (ignored by the menu) |
| Touch | `sec_touchscreen`, `BTN_TOUCH` |
| Wi-Fi | `dhd`/`ifxdhd`/`bcm43xx` modules from `modules.load.recovery`, loaded by `init` |
| MAC address | random `00:12:34:xx:xx:xx` on every boot |

## Tests

| Test | Result |
|---|---|
| Boot into the recovery, Samsung UI replaced by `gw4ui` | passed |
| Home network (WPA2/WPA3 transition network, `WPA-PSK`), DHCP, host name `gw4-recovery` | passed |
| `adbd` as root on TCP 5555, no key | passed (`uid=0`, `u:r:su:s0`) |
| Signal level on screen | passed (`mf_cli signal_poll`) |
| Writing the recovery partition with `dd` from inside this recovery, read-back hash | passed, twice |
| Wireless Download with this recovery on the partition | starts and accepts a client (owner's report); no full NetOdin flash tried |
| Fallback access point `GW4-RECOVERY` | not tested |
| Menu actions on the watch | not checked one by one |

SELinux stays `Enforcing`.
