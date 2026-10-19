#!/system/bin/sh
# Magisk "late_start service" script: runs once on every boot.
#
# It starts powerfilter in a small restart loop:
#   - the power key is found by NAME (qpnp_pon), not by /dev/input/eventN,
#     because the event number can change between boots;
#   - if powerfilter ever exits or crashes, the kernel gives the real device
#     back automatically (the button works as it did before, just unfiltered)
#     and the loop starts it again a few seconds later.
#
# KILL SWITCH (stop it and keep it stopped, no uninstall needed):
#     touch /data/adb/powerfilter.disable && pkill powerfilter
# Turn it back on:
#     rm /data/adb/powerfilter.disable   (then reboot, or run this script)

MODDIR=${0%/*}
BIN=$MODDIR/bin/powerfilter
DEVICE_NAME=qpnp_pon          # name shown by `getevent -lp` for the power key
DELAY_MS=70                   # releases are held this long; re-presses inside it are ignored
DISABLE=/data/adb/powerfilter.disable
LOG=/data/local/tmp/powerfilter.log

chmod 755 "$BIN"
: > "$LOG"                    # start with a fresh log each boot

# Run in the background so we never block the boot process.
(
  while [ ! -e "$DISABLE" ]; do
    "$BIN" -n "$DEVICE_NAME" "$DELAY_MS" >> "$LOG" 2>&1
    sleep 3                   # avoid a tight loop if it keeps failing
  done
) &
