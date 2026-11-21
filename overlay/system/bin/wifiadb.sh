#!/system/bin/sh
# Brings up ADB over Wi-Fi in the normal recovery mode and shows the progress
# on the screen through gw4ui (it replaces the Samsung recovery UI; the menu
# entry "Samsung recovery" brings the original one back).
# 1. Home network: /cache/wifi-adb/wpa.conf (wpa_supplicant format), if present.
# 2. Fallback: own WPA2 access point GW4-RECOVERY-XXXX. The suffix (four hex
#    digits, like the SM-R870-XXXX of Wireless Download) and the password are
#    generated on the first use and kept in /cache/wifi-adb, so a laptop can
#    remember the network. /cache/wifi-adb/hostapd.conf overrides everything.
# ADB listens on TCP 5555 in both cases.
# The script never exits after setup: init kills the whole process group of a
# finished oneshot service, which would take the supplicant or hostapd with it.
LOG=/tmp/wifiadb.log
STATUS=/tmp/wifiadb.status
exec >>"$LOG" 2>&1
echo "wifiadb: start $(date)"

# Status lines for gw4ui, one argument per line.
status() {
  printf '%s\n' "$@" > "$STATUS.new" && mv "$STATUS.new" "$STATUS"
  echo "wifiadb: $*"
}

# Keeps the script alive and, on the home network, refreshes the signal level
# for gw4ui in /tmp/wifiadb.rssi.
stay() {
  while :; do
    if [ -S /tmp/wpa/wlan0 ]; then
      mf_cli -p /tmp/wpa -i wlan0 signal_poll 2>/dev/null | sed -n 's/^RSSI=//p' > /tmp/wifiadb.rssi
    fi
    sleep 5
  done
}

# Give the recovery binary time to decide between normal and wireless mode.
sleep 8
if [ -n "$(getprop ro.boot.wirelessd)" ] || pidof wirelessd >/dev/null; then
  echo "wifiadb: wireless download mode, leaving wlan0 alone"
  exit 0
fi
if head -c 64 /dev/block/by-name/misc 2>/dev/null | grep -qa wireless; then
  echo "wifiadb: BCB requests wireless mode, leaving wlan0 alone"
  exit 0
fi

status "Starting Wi-Fi..."
setprop ctl.stop recovery
sleep 1
setprop ctl.start wifiadb_ui

# Copy the optional configs from /cache without keeping it mounted.
mkdir -p /tmp/wifiadb /tmp/wcache /tmp/wpa /tmp/hostapd
if mount -t ext4 -o ro /dev/block/by-name/cache /tmp/wcache; then
  cp /tmp/wcache/wifi-adb/wpa.conf /tmp/wifiadb/ 2>/dev/null
  cp /tmp/wcache/wifi-adb/hostapd.conf /tmp/wifiadb/ 2>/dev/null
  umount /tmp/wcache
fi

i=0
while [ ! -d /sys/class/net/wlan0 ] && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done
if [ ! -d /sys/class/net/wlan0 ]; then
  status "No wlan0" "Wi-Fi driver failed"
  stay
fi
# The driver makes up a random MAC on every boot (it cannot read the real one
# from /efs here) and overrides any address set before the first ifup. The
# router registers the DHCP hostname, so connect by name: gw4-recovery.lan.
ip link set wlan0 up

start_adb() {
  setprop service.adb.tcp.port 5555
  setprop ctl.restart adbd
  ADDR=$(ip -4 addr show wlan0 | grep -o 'inet [0-9.]*' | cut -d' ' -f2)
  echo "wifiadb: adb on $ADDR:5555"
}

if [ -f /tmp/wifiadb/wpa.conf ]; then
  SSID=$(sed -n 's/^[[:space:]]*ssid="\(.*\)"/\1/p' /tmp/wifiadb/wpa.conf | head -1)
  status "Wi-Fi: $SSID" "Connecting..."
  mf_supplicant -B -i wlan0 -D nl80211 -c /tmp/wifiadb/wpa.conf -P /tmp/wpa/pid
  status "Wi-Fi: $SSID" "Waiting for DHCP..."
  if dhcp -i wlan0 -s /system/bin/udhcp_script.mf -t 10 -T 3 -n -q -H gw4-recovery; then
    start_adb
    status "Wi-Fi: $SSID" "ADB ready:" "$ADDR:5555"
    stay
  fi
  echo "wifiadb: home network failed, switching to access point"
  kill "$(cat /tmp/wpa/pid)" 2>/dev/null
  sleep 1
fi

# Prints N random characters from an alphabet without look-alikes (0/o, 1/l/i).
rand_chars() {
  od -An -tu1 -N"$1" /dev/urandom | tr -s ' ' '\n' | sed '/^$/d' | while read -r b; do
    printf '%s' "$(echo abcdefghjkmnpqrstuvwxyz23456789 | cut -c$((b % 31 + 1)))"
  done
}

# Access point name suffix and password: read from /cache, or generate and
# store them there. Without a writable /cache they live only for this boot.
ap_identity() {
  if mount -t ext4 /dev/block/by-name/cache /tmp/wcache; then
    mkdir -p /tmp/wcache/wifi-adb && chmod 700 /tmp/wcache/wifi-adb
    if [ ! -s /tmp/wcache/wifi-adb/ap-id ] || [ ! -s /tmp/wcache/wifi-adb/ap-pass ]; then
      (umask 077
       od -An -tx1 -N2 /dev/urandom | tr -d ' \n' | tr a-f A-F > /tmp/wcache/wifi-adb/ap-id
       echo "$(rand_chars 5)-$(rand_chars 5)" > /tmp/wcache/wifi-adb/ap-pass)
      echo "wifiadb: new access point identity stored in /cache"
    fi
    AP_ID=$(cat /tmp/wcache/wifi-adb/ap-id)
    AP_PASS=$(cat /tmp/wcache/wifi-adb/ap-pass)
    sync
    umount /tmp/wcache
  fi
  if [ -z "$AP_ID" ] || [ -z "$AP_PASS" ]; then
    AP_ID=$(od -An -tx1 -N2 /dev/urandom | tr -d ' \n' | tr a-f A-F)
    AP_PASS="$(rand_chars 5)-$(rand_chars 5)"
    echo "wifiadb: /cache not writable, temporary access point identity"
  fi
}

if [ -f /tmp/wifiadb/hostapd.conf ]; then
  AP=/tmp/wifiadb/hostapd.conf
else
  ap_identity
  AP=/tmp/wifiadb/ap.conf
  (umask 077
   cat /vendor/etc/wifi/wifiadb-ap.conf
   echo "ssid=GW4-RECOVERY-$AP_ID"
   echo "wpa_passphrase=$AP_PASS") > "$AP"
fi
AP_SSID=$(sed -n 's/^ssid=//p' "$AP")
AP_PASS=$(sed -n 's/^wpa_passphrase=//p' "$AP")
status "Starting access point..."
hostapd -B "$AP"
sleep 2
ip addr add 192.168.49.1/24 dev wlan0
dhcpd /vendor/etc/wifi/dhcpd.conf
start_adb
status "Access point:" "$AP_SSID" "Pass: $AP_PASS" "ADB 192.168.49.1:5555"
stay
