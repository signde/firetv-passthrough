#!/system/bin/sh
# Shared opt-in interface. Opening this node does not start an MVC 3D session.
#
# The SELinux types and rules come from the module's sepolicy.rule (written at install from
# mvc3d_policy_rules), which Magisk applies early in boot, before system_server starts. A live policy change
# right after boot can crash system_server: each one reloads the whole policy, and a HAL lookup during the
# reload fails ("gralloc-mapper is missing", SIGABRT; seen 2026-11-26, after which Fire OS dropped every
# app's display mode request). So the boot only relabels the nodes; the live change is a fallback, for a boot
# policy without the types (a Magisk that does not apply sepolicy.rule), and only once the system has settled.
. "$OWNER/scripts/live-policy.sh"
MVC3D_DECODER_TYPE=firetv_mvc3d_decoder_device

# the rules for /dev/mvc3dhw and for the decoder input (its stock type, arg 1: that type's character device
# rules, copied from the running policy, and app access)
mvc3d_policy_rules() {
    echo "type firetv_mvc3d_device file_type"
    echo "typeattribute firetv_mvc3d_device mlstrustedobject"
    echo "allow appdomain firetv_mvc3d_device chr_file { read write open getattr ioctl }"
    [ -n "$1" ] || return 0
    echo "type $MVC3D_DECODER_TYPE dev_type"
    echo "typeattribute $MVC3D_DECODER_TYPE mlstrustedobject"
    if [ "$1" != "$MVC3D_DECODER_TYPE" ]; then
        magiskpolicy --print-rules 2>/dev/null |
            sed -n "s/^allow \([^ ]*\) $1 chr_file \(.*\)\$/allow \1 $MVC3D_DECODER_TYPE chr_file \2/p"
    else
        magiskpolicy --print-rules 2>/dev/null | grep "^allow [^ ]* $MVC3D_DECODER_TYPE chr_file "
    fi
    echo "allow appdomain $MVC3D_DECODER_TYPE chr_file { read write open getattr ioctl }"
}

# type: 0 if the running policy has it. Writing the context to selinuxfs checks it; chcon does not: root
# may set a context the policy lacks, and the node is then "unlabeled" to everyone (seen 2026-11-26 on a
# Magisk without a preinit partition, which does not apply sepolicy.rule).
mvc3d_type_known() {
    (echo -n "u:object_r:$1:s0" > /sys/fs/selinux/context) 2>/dev/null
}

# node type: 0 if the policy has the type and the node now has it
mvc3d_relabel() {
    mvc3d_type_known "$2" && chcon "u:object_r:$2:s0" "$1" 2>/dev/null
}

# before a live policy change: well after boot, once system_server's start-up is over
mvc3d_settle() {
    up=$(cut -d. -f1 /proc/uptime)
    [ "$up" -ge 90 ] || sleep $((90 - up))
}

provision_mvc3d_access() {
    [ -c /dev/mvc3dhw ] || return 1
    chmod 000 /dev/mvc3dhw || return 1
    chown 0:0 /dev/mvc3dhw || return 1
    if ! mvc3d_relabel /dev/mvc3dhw firetv_mvc3d_device; then
        echo "MVC 3D device: no type in the boot policy, applying it live once settled"
        mvc3d_settle
        live_policy 'type firetv_mvc3d_device file_type' \
            'typeattribute firetv_mvc3d_device mlstrustedobject' \
            'allow appdomain firetv_mvc3d_device chr_file { read write open getattr ioctl }' || return 1
        mvc3d_relabel /dev/mvc3dhw firetv_mvc3d_device || return 1
    fi
    chmod 666 /dev/mvc3dhw || return 1
}

# A player feeds the stock hardware MVC decoder itself (libmvc3dplay's hardware path), through
# /dev/amstream_vbuf. Apps cannot use it on stock Fire OS: its stock type (codec_device on PS7718) is
# shared by some 27 media nodes and is not MLS-trusted, so an app (with its own MLS categories) cannot
# write it even with an allow rule. The node gets its own type instead, MLS-trusted like /dev/mvc3dhw:
# it keeps every rule the stock type has on character devices (the media services that use the decoder
# keep their access) and adds app access, for this node only. ENABLE_MVC3D_APP_DECODER=0 leaves it out
# (players then need root for the decoder input).
provision_mvc3d_decoder_access() {
    n=/dev/amstream_vbuf
    new=$MVC3D_DECODER_TYPE
    [ -c "$n" ] || return 1
    t=$(ls -Z "$n" | sed -n 's/^[^:]*:[^:]*:\([^:]*\):.*/\1/p')
    [ -n "$t" ] || return 1
    how="boot policy"
    if [ "$t" != "$new" ] && ! mvc3d_relabel "$n" "$new"; then
        echo "decoder input: no $new in the boot policy, applying it live once settled"
        mvc3d_settle
        rules=${TMPDIR:-/dev}/mvc3d-decoder.rules
        mvc3d_policy_rules "$t" | grep -v firetv_mvc3d_device > "$rules" || return 1
        # at least the app rule and one stock rule (a stock service that uses the decoder)
        [ "$(grep -c "^allow " "$rules")" -ge 2 ] || { rm -f "$rules"; return 1; }
        live_policy --apply "$rules" || { rm -f "$rules"; return 1; }
        rm -f "$rules"
        mvc3d_relabel "$n" "$new" || return 1
        how="applied live"
    fi
    mode=$(stat -c %a "$n") || return 1
    case "$mode" in
        *6) ;;
        *) chmod o+rw "$n" || return 1 ;;
    esac
    echo "decoder input $n: type $t -> $new ($how), mode $mode -> $(stat -c %a "$n")"
}
