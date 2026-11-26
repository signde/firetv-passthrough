#!/system/bin/sh
# Live SELinux policy changes, one at a time. selinuxfs lets one process at a time read the loaded policy
# (another gets EBUSY, and magiskpolicy aborts), and this module's loaders (FEL, MVC 3D) run side by side at
# boot: each change holds /dev/firetv_policy.lock (tmpfs) for its magiskpolicy run. A change that still
# fails - another module's magiskpolicy reading the policy at that moment - is tried again.
live_policy() {
    tries=1
    until ( /data/adb/magisk/busybox flock -x 8 && magiskpolicy --live "$@" ) 8>/dev/firetv_policy.lock; do
        [ "$tries" -lt 5 ] || return 1
        echo "live policy change failed (try $tries of 5), trying again" >&2
        sleep 1
        tries=$((tries + 1))
    done
}
