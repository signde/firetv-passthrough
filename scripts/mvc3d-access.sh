#!/system/bin/sh
# Shared opt-in interface. Opening this node does not start an MVC 3D session.
provision_mvc3d_access() {
    [ -c /dev/mvc3dhw ] || return 1
    chmod 000 /dev/mvc3dhw || return 1
    magiskpolicy --live 'type firetv_mvc3d_device file_type' \
        'typeattribute firetv_mvc3d_device mlstrustedobject' \
        'allow appdomain firetv_mvc3d_device chr_file { read write open getattr ioctl }' || return 1
    chown 0:0 /dev/mvc3dhw || return 1
    chcon u:object_r:firetv_mvc3d_device:s0 /dev/mvc3dhw || return 1
    chmod 666 /dev/mvc3dhw || return 1
}

# A player feeds the stock hardware MVC decoder itself (libmvc3dplay's hardware path), through
# /dev/amstream_vbuf. Apps cannot open it on stock Fire OS: this lets them, as the helper's own node.
# It widens the decoder's input to every app (as /dev/dvfel is open to every app); the node keeps its
# stock SELinux type, which gets an allow rule for app domains. ENABLE_MVC3D_APP_DECODER=0 leaves it
# out (players then need root for the decoder input).
provision_mvc3d_decoder_access() {
    n=/dev/amstream_vbuf
    [ -c "$n" ] || return 1
    t=$(ls -Z "$n" | sed -n 's/^[^:]*:[^:]*:\([^:]*\):.*/\1/p')
    [ -n "$t" ] || return 1
    magiskpolicy --live "allow appdomain $t chr_file { read write open getattr ioctl }" || return 1
    mode=$(stat -c %a "$n") || return 1
    case "$mode" in
        *6) ;;
        *) chmod o+rw "$n" || return 1 ;;
    esac
    echo "decoder input $n: type $t, mode $mode -> $(stat -c %a "$n")"
}
