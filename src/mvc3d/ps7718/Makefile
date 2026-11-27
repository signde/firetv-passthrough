# SPDX-License-Identifier: GPL-2.0-or-later
# mvc3d: out-of-tree module for the Fire TV Cube 3rd gen (Fire OS 7.7.1.8 PS7718, 5.4.125-ab35).
# See README.md for the build.
obj-m += mvc3d.o
mvc3d-y := mvc3d_main.o mvc3d_fp.o mvc3d_pool.o mvc3d_sys.o mvc3d_ui.o
# mvc3d_ui.c: the framebuffer OSD driver's frame info (osd/osd.h, osd/osd_sync.h)
CFLAGS_mvc3d_ui.o += -I$(srctree)/drivers/amlogic/media
