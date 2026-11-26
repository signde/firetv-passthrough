#!/system/bin/sh
# Shared opt-in interface. Opening this node does not start an MVC 3D session.
. "$OWNER/scripts/live-policy.sh"
provision_mvc3d_access() {
    [ -c /dev/mvc3dhw ] || return 1
    chmod 000 /dev/mvc3dhw || return 1
    live_policy 'type firetv_mvc3d_device file_type' \
        'typeattribute firetv_mvc3d_device mlstrustedobject' \
        'allow appdomain firetv_mvc3d_device chr_file { read write open getattr ioctl }' || return 1
    chown 0:0 /dev/mvc3dhw || return 1
    chcon u:object_r:firetv_mvc3d_device:s0 /dev/mvc3dhw || return 1
    chmod 666 /dev/mvc3dhw || return 1
}

# A player feeds the stock hardware MVC decoder itself (libmvc3dplay's hardware path), through
# /dev/amstream_vbuf. Apps cannot use it on stock Fire OS: its stock type (codec_device on PS7718) is
# shared by some 27 media nodes and is not MLS-trusted, so an app (with its own MLS categories) cannot
# write it even with an allow rule. The node gets its own type instead, MLS-trusted like /dev/mvc3dhw:
# it keeps every rule the stock type has on character devices (the media services that use the decoder
# keep their access) and adds app access, for this node only. ENABLE_MVC3D_APP_DECODER=0 leaves it out
# (players then need root for the decoder input).
MVC3D_DECODER_TYPE=firetv_mvc3d_decoder_device
provision_mvc3d_decoder_access() {
    n=/dev/amstream_vbuf
    new=$MVC3D_DECODER_TYPE
    [ -c "$n" ] || return 1
    t=$(ls -Z "$n" | sed -n 's/^[^:]*:[^:]*:\([^:]*\):.*/\1/p')
    [ -n "$t" ] || return 1
    n_rules=1
    if [ "$t" != "$new" ]; then
        rules=${TMPDIR:-/dev}/mvc3d-decoder.rules
        {
            echo "type $new dev_type"
            echo "typeattribute $new mlstrustedobject"
            # the stock type's character device rules, for the new type
            magiskpolicy --print-rules 2>/dev/null |
                sed -n "s/^allow \([^ ]*\) $t chr_file \(.*\)\$/allow \1 $new chr_file \2/p"
            echo "allow appdomain $new chr_file { read write open getattr ioctl }"
        } > "$rules" || return 1
        n_rules=$(grep -c "^allow " "$rules")
        # at least the app rule and one stock rule (a stock service that uses the decoder)
        [ "$n_rules" -ge 2 ] || { rm -f "$rules"; return 1; }
        live_policy --apply "$rules" || { rm -f "$rules"; return 1; }
        rm -f "$rules"
        chcon "u:object_r:$new:s0" "$n" || return 1
    fi
    mode=$(stat -c %a "$n") || return 1
    case "$mode" in
        *6) ;;
        *) chmod o+rw "$n" || return 1 ;;
    esac
    echo "decoder input $n: type $t -> $new ($((n_rules - 1)) stock rules kept), mode $mode -> $(stat -c %a "$n")"
}
