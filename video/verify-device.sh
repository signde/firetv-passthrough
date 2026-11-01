#!/system/bin/sh
verify_video_device() {
 [ "$(getprop ro.product.device)" = gazelle ] || return 1
 case "$(getprop ro.build.fingerprint)" in
  */PS7702.4965N/*) FEL_PROFILE=ps7702; EXPECTED_KERNEL=5.4.125-ab27;;
  */PS7717.5741N/*) FEL_PROFILE=ps7717; EXPECTED_KERNEL=5.4.125-ab25;;
  *) return 1;;
 esac
 [ "$(uname -r)" = "$EXPECTED_KERNEL" ] || return 1
 actual=$(sha256sum /vendor/lib/modules/dovi_vs12.ko) || return 1
 [ "${actual%% *}" = 7c59782906328a9a524719ac0df18ea5d6395d901ca035e3ad7cfed7a9025224 ]
}
