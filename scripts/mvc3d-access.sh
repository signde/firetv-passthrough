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
