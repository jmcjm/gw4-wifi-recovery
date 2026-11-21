# Lessons learned

## Wireless Download lives in the recovery partition

The yellow "DOWNLOAD MODE / Wireless-download" screen is not the bootloader. The bootloader starts the recovery kernel and ramdisk with `ro.boot.wirelessd=1` (or `2`); the `recovery` binary then runs Samsung's `wirelessd` code, which starts a hotspot (`hostapd` + `dhcpd`, SSID `SM-R870-xxxx`), speaks the Odin protocol and writes to `/dev/block/mmcblk0` itself. The signature and rollback checks that make NetOdin reject unsigned images (`sig check`, `[EFUSE] RB Count`) are in that userspace code.

Consequences:

- A broken recovery partition also breaks Wireless Download. Flash a new recovery only while the system boots, and verify the read-back hash.
- The additions must not touch Wi-Fi in Wireless Download mode. `wifiadb.sh` checks `ro.boot.wirelessd`, a running `wirelessd` process and the bootloader message in `misc` before doing anything.

## init kills the process group of a finished oneshot service

The first version ended `wifiadb.sh` right after DHCP. `init` then killed the whole process group of the service, including the backgrounded `mf_supplicant`, and the watch dropped off the network one second after getting its address. The script now stays alive in a loop.

## adbd drops root unless the build looks like a debug build

With `ro.secure=1` and `ro.debuggable=0` from the stock `prop.default`, `adbd` runs the shell as uid 2000 even though the service has `--root_seclabel=u:r:su:s0`. Setting `ro.secure=0` and `ro.debuggable=1` keeps root.

## setenforce does not work

`setenforce 0` has no effect on this kernel; SELinux stays `Enforcing`. The domains involved are marked permissive in the policy instead (`sepolicy/rules.te`). `init` also needs explicit rules to start a shell script or `gw4ui` in the `recovery` domain.

## The MAC address cannot be fixed from userspace

The Wi-Fi driver cannot read the real address from `/efs` in recovery, makes up a random `00:12:34:xx:xx:xx`, and overrides any address set before the first `ifup`. The router's DNS entry for the DHCP host name (`gw4-recovery.lan`) solves the addressing problem instead.

## The display is fbdev

There is no DRM device on this watch; `gw4ui` draws into `/dev/graphics/fb0` and pans to offset 0. The Wi-Fi driver reports zero signal in `/proc/net/wireless`, so the level comes from `mf_cli signal_poll` through the supplicant control socket in `/tmp/wpa`.

## Repacking on the watch is slow

`magiskboot repack` on the watch decompresses and recompresses the 37 MB kernel on one core and takes about five minutes. The x86_64 `magiskboot` from the Magisk APK runs on Linux; with `unpack -n` and `repack -n` it leaves the compressed kernel untouched and the build takes seconds.

## Stopping the Android framework drops wireless ADB

Wireless debugging is handled by `system_server`. Running `stop` over a wireless ADB session cuts the session before any following `start` runs, and the watch stays without a UI until it is rebooted. Tests that need the framework stopped must run detached (`setsid`/`nohup`) and restart it themselves.

## Wireless Download picks a new SSID every session

The `SM-R870-XXXX` name of Wireless Download is random per session (`SM-R870-93BE` and `SM-R870-8463` on the same evening, from `/cache/recovery/last_log*`), not derived from the MAC address, serial number or `ro.boot.em.did`. The fallback access point of this recovery uses the same four-hex-digit format but keeps it in `/cache`, so the network stays the same between boots.

## /proc/net/wireless triggers a kernel warning

Reading `/proc/net/wireless` in recovery logs a kernel warning from `wireless_dev_seq_start`, and the `dhd` driver reports zeros there anyway. `gw4ui` reads only the RSSI that `wifiadb.sh` writes.

## mDNS finds the watch from the desktop

`adb mdns services` on a machine in the same LAN lists the watch as `_adb-tls-connect._tcp` with its current address and port, so the changing wireless debugging port does not have to be read off the screen.
