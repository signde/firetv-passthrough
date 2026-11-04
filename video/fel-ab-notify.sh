#!/system/bin/sh
# Follows dvfel's el_mute (the A/B button: the remote's Netflix button during FEL): on each change it asks the video driver to set
# the shown frames up again (force_property_change: dvfel has already re-pointed the shown EL, and this
# applies it to a paused picture; the kernel's own write is refused by SELinux), then shows "FEL: EL off"
# / "FEL: EL on" through the FEL indicator helper app (fel-indicator.apk; installed here when missing).
# The players are not changed. Started in the background by fel-service.sh.
M=$1
PKG=com.firetvav.felindicator
P=/sys/module/dvfel/parameters/el_mute
pm path $PKG >/dev/null 2>&1 || pm install -r "$M/fel-indicator.apk" >/dev/null 2>&1
[ -r $P ] || exit 0
read last < $P
while [ -r $P ]; do
 read v < $P
 if [ "$v" != "$last" ]; then
  last=$v
  echo 1 > /sys/class/video/force_property_change 2>/dev/null
  off=0; [ "$v" = Y ] && off=1
  am broadcast -f 0x20 -a $PKG.EL -n $PKG/.ElReceiver --ei off $off >/dev/null 2>&1
 fi
 sleep 0.1
done
