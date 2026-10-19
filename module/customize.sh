#!/system/bin/sh
# Sourced by the Magisk installer after the zip is extracted to $MODPATH.
ui_print "- Power Button Debounce"
ui_print "- Ignores a power-key release+press within 70 ms"
ui_print "- Kill switch: touch /data/adb/powerfilter.disable && pkill powerfilter"

# Default permissions for everything, then make the binary and script executable.
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/powerfilter" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
