#!/system/bin/sh
# Shared opt-in interface. Opening this node does not start a FEL session.
provision_fel_access() {
    [ -c /dev/dvfel ] || return 1
    chmod 000 /dev/dvfel || return 1
    magiskpolicy --live 'type firetv_fel_device file_type' \
        'typeattribute firetv_fel_device mlstrustedobject' \
        'allow appdomain firetv_fel_device chr_file { read write open getattr ioctl }' || return 1
    chown 0:0 /dev/dvfel || return 1
    chcon u:object_r:firetv_fel_device:s0 /dev/dvfel || return 1
    chmod 666 /dev/dvfel || return 1
}
