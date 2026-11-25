# Building and flashing

## 1. Tools

```bash
./build.sh --tools Magisk-v31.1.apk   # extracts the x86_64 magiskboot into tools/
```

`magiskboot` from the APK is statically linked and runs on a normal Linux system. `magiskpolicy` from the same APK is built against Android's libc and does not, which is why the policy is patched on the watch.

Also needed: `adb`, `lz4`, `unzip` and the Android NDK (set `NDK` to its root).

## 2. Stock recovery

Dump it from the rooted watch (Android with Magisk):

```bash
adb shell "su -c 'dd if=/dev/block/by-name/recovery of=/data/local/tmp/recovery-stock.img bs=1M'"
adb pull /data/local/tmp/recovery-stock.img
adb shell "su -c 'rm /data/local/tmp/recovery-stock.img'"
sha256sum recovery-stock.img
```

Without root, take it from the firmware instead: `tar -xOf AP_..._WATCH_OS16.tar.md5 recovery.img.lz4 | lz4 -d > recovery-stock.img` gives the recovery image of that firmware. Only use the AP of the firmware the watch runs.

Keep this file. It is the only way back to the stock recovery, and with it to an untouched Wireless Download, without a full firmware flash. For `R870XXS2JZG3` the expected hash is in `docs/STATUS.md`.

## 3. Policy

```bash
./build.sh --extract-sepolicy recovery-stock.img sepolicy.stock
sepolicy/patch-sepolicy.sh <adb serial> sepolicy.stock sepolicy.patched
```

The watch only runs `magiskpolicy` on the file, so any watch with Magisk will do, also another model. The script pushes the policy to the watch, applies `sepolicy/rules.te` with `/data/adb/magisk/magiskpolicy`, pulls the result back and removes the temporary files.

## 4. Image

```bash
NDK=/path/to/ndk ./build.sh recovery-stock.img sepolicy.patched recovery-wifiadb.img
```

The script builds `gw4ui`, adds the overlay to the ramdisk, repacks the image and checks that the kernel, `dtb` and `recovery_dtbo` are unchanged.

## 5. Home network config

The recovery reads `/cache/wifi-adb/wpa.conf`. Write it from Android as root, for example:

```
ctrl_interface=/tmp/wpa
update_config=0
network={
	ssid="MyNetwork"
	psk="password"
	key_mgmt=WPA-PSK
}
```

Keep the file mode at `600`. `ctrl_interface=/tmp/wpa` is required: the script reads the signal level through that socket. Plain `WPA-PSK` works with WPA2/WPA3 transition networks too.

## 6. Flashing

From Android with root:

```bash
adb push recovery-wifiadb.img /data/local/tmp/
adb shell "su -c 'dd if=/data/local/tmp/recovery-wifiadb.img of=/dev/block/by-name/recovery bs=1M conv=fsync && sync'"
adb shell "su -c 'dd if=/dev/block/by-name/recovery bs=1M count=53 | sha256sum'"   # compare with the image
adb reboot recovery
```

`count=53` matches the 55 574 528-byte image of `R870XXS2JZG3`; adjust it for an image of another size.

From this recovery itself (it runs from RAM, so overwriting its own partition is safe):

```bash
adb connect gw4-recovery.lan:5555
adb -s gw4-recovery.lan:5555 push recovery-wifiadb.img /tmp/
adb -s gw4-recovery.lan:5555 shell "dd if=/tmp/recovery-wifiadb.img of=/dev/block/by-name/recovery bs=1M conv=fsync && sync"
```

Check the read-back hash before rebooting. A broken recovery partition takes Wireless Download with it.

## Going back

Write `recovery-stock.img` the same way.
