// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mvc3d: the display of a frame packed session, set up and put back in-kernel.
 *
 * What a root script did around each 3D session (save the HDMI mode and colour attribute, the UI
 * planes and the video layer state, switch to 1080p24 frame packed, hide the UI planes; all of it
 * back afterwards) is done here, at MVC3D_HW_START and at the session's stop or the last close of its
 * file (an app that crashes included). Players then need no root.
 *
 * The sysfs attributes are written through their own sysfs_ops (the same store/show functions a
 * write or read of the file calls): SELinux does not let a kernel thread open these files, and an
 * ioctl caller is an app. The attribute is found from the sysfs root (reached from this module's own
 * directory) by its path, following symlinks; the classes and devices used here (display, video,
 * graphics fb0-2, amhdmitx0) exist for the whole uptime.
 */
#include "mvc3d.h"

#define SYS_MODE "class/display/mode"
#define SYS_ATTR "class/amhdmitx/amhdmitx0/attr"
#define SYS_VGO "class/video/video_global_output"
#define SYS_DISABLE "class/video/disable_video"
#define FP_MODE "1080p24hz"

static int own_display = 1;
module_param(own_display, int, 0644);
MODULE_PARM_DESC(
	own_display,
	"1: a frame packed session's display (mode, UI planes, video layer) is set up and put back by the module");

bool mvc3d_own_display(void)
{
	return READ_ONCE(own_display) != 0;
}

/* the sysfs root: the top of our own /sys/module/mvc3d */
static struct kernfs_node *sys_root(void)
{
	struct kernfs_node *kn = THIS_MODULE->mkobj.kobj.sd;

	while (kn && kn->parent)
		kn = kn->parent;
	return kn;
}

/* "class/display/mode": its node, symlinks followed, with a reference; NULL if not there */
static struct kernfs_node *sys_lookup(const char *path)
{
	struct kernfs_node *kn = sys_root(), *next;
	char buf[96], *p = buf, *name;

	if (!kn || strscpy(buf, path, sizeof(buf)) < 0)
		return NULL;
	kernfs_get(kn);
	while ((name = strsep(&p, "/"))) {
		if (!*name)
			continue;
		next = kernfs_find_and_get(kn, name);
		kernfs_put(kn);
		if (!next)
			return NULL;
		if (kernfs_type(next) == KERNFS_LINK) {
			struct kernfs_node *t = next->symlink.target_kn;

			kernfs_get(t);
			kernfs_put(next);
			next = t;
		}
		kn = next;
	}
	return kn;
}

/* set: the value written (as a shell's echo, with its newline); get: the value read, one line */
static ssize_t sys_attr(const char *path, const char *set, char *get, size_t gsz)
{
	struct kernfs_node *kn = sys_lookup(path);
	const struct sysfs_ops *ops;
	struct kobject *kobj;
	struct attribute *attr;
	ssize_t r = -EINVAL;

	if (get && gsz)
		get[0] = 0;
	if (!kn)
		return -ENOENT;
	if (kernfs_type(kn) == KERNFS_FILE && kn->priv && kn->parent && kn->parent->priv) {
		kobj = kn->parent->priv;
		attr = kn->priv;
		ops = kobj->ktype ? kobj->ktype->sysfs_ops : NULL;
		if (ops) {
			kobject_get(kobj);
			if (set) {
				char v[64];

				snprintf(v, sizeof(v), "%s\n", set);
				r = ops->store ? ops->store(kobj, attr, v, strlen(v)) : -EPERM;
			} else {
				char *b = (char *)get_zeroed_page(GFP_KERNEL);

				r = -ENOMEM;
				if (b) {
					r = ops->show ? ops->show(kobj, attr, b) : -EPERM;
					if (r >= 0 && get && gsz) {
						strscpy(get, b, gsz);
						get[strcspn(get, "\n")] = 0;
					}
					free_page((unsigned long)b);
				}
			}
			kobject_put(kobj);
		}
	}
	kernfs_put(kn);
	return r;
}

int mvc3d_sys_read(const char *path, char *buf, size_t size)
{
	ssize_t r = sys_attr(path, NULL, buf, size);

	if (r < 0)
		pr_warn("read %s: %zd\n", path, r);
	return r < 0 ? r : 0;
}

int mvc3d_sys_write(const char *path, const char *val)
{
	ssize_t r = sys_attr(path, val, NULL, 0);

	if (r < 0)
		pr_warn("%s <- %s: %zd\n", path, val, r);
	return r < 0 ? r : 0;
}

/* the display as it was before the session */
static struct {
	bool taken;
	char mode[32], attr[32], vgo[8], fps[8], dve[8], osd[3][8];
} disp;

static DEFINE_MUTEX(disp_lock);

/* osd_display_debug as the script kept it: its first digits (at most 3), else 0 */
static void osd_get(int i, char *v, size_t vsz)
{
	char path[48], raw[64], *s;
	size_t n = 0;

	snprintf(path, sizeof(path), "class/graphics/fb%d/osd_display_debug", i);
	sys_attr(path, NULL, raw, sizeof(raw));
	for (s = raw; *s && n < 3 && n + 1 < vsz; s++)
		if (*s >= '0' && *s <= '9')
			v[n++] = *s;
	if (!n)
		v[n++] = '0';
	v[n] = 0;
}

static void osd_set(int i, const char *debug, const char *blank)
{
	char path[48];

	snprintf(path, sizeof(path), "class/graphics/fb%d/osd_display_debug", i);
	sys_attr(path, debug, NULL, 0);
	snprintf(path, sizeof(path), "class/graphics/fb%d/blank", i);
	sys_attr(path, blank, NULL, 0);
}

/* (disp_lock held) everything back as it was */
static void display_restore(void)
{
	char v[32];
	int i;

	for (i = 0; i < 3; i++)
		osd_set(i, disp.osd[i], "0");
	if (disp.vgo[0])
		mvc3d_sys_write(SYS_VGO, disp.vgo);
	mvc3d_sys_write(SYS_DISABLE, "0");
	/* the next mode set is 2D */
	mvc3d_fp_arm(false);
	msleep(1000);
	mvc3d_sys_write(SYS_MODE, disp.mode);
	msleep(2000);
	if (disp.attr[0] && !mvc3d_sys_read(SYS_ATTR, v, sizeof(v)) && strcmp(v, disp.attr))
		mvc3d_sys_write(SYS_ATTR, disp.attr);
	if (disp.fps[0])
		mvc3d_kparam("aml_media", "framepacking_support", disp.fps, NULL, 0);
	if (disp.dve[0] && !mvc3d_kparam("aml_media", "dolby_vision_enable", NULL, v, sizeof(v)) &&
	    strcmp(v, disp.dve))
		mvc3d_kparam("aml_media", "dolby_vision_enable", disp.dve, NULL, 0);
	disp.taken = false;
	mvc3d_sys_read(SYS_MODE, v, sizeof(v));
	pr_info("display back: mode %s\n", v);
}

/*
 * (process context; frame packing hooks on) the display saved, 1080p24 frame packed, UI planes
 * hidden, the video layer on: 0, or an error with everything put back
 */
int mvc3d_display_take(void)
{
	int i, r = 0;

	mutex_lock(&disp_lock);
	if (disp.taken)
		goto out;
	if (mvc3d_sys_read(SYS_MODE, disp.mode, sizeof(disp.mode)) || !disp.mode[0]) {
		r = -EIO;
		goto out;
	}
	mvc3d_sys_read(SYS_ATTR, disp.attr, sizeof(disp.attr));
	mvc3d_sys_read(SYS_VGO, disp.vgo, sizeof(disp.vgo));
	mvc3d_kparam("aml_media", "framepacking_support", NULL, disp.fps, sizeof(disp.fps));
	mvc3d_kparam("aml_media", "dolby_vision_enable", NULL, disp.dve, sizeof(disp.dve));
	for (i = 0; i < 3; i++)
		osd_get(i, disp.osd[i], sizeof(disp.osd[i]));
	pr_info("display before: mode %s attr %s video_global_output %s framepacking_support %s dolby_vision_enable %s osd %s %s %s\n",
		disp.mode, disp.attr, disp.vgo, disp.fps, disp.dve, disp.osd[0], disp.osd[1],
		disp.osd[2]);
	disp.taken = true;
	mvc3d_fp_arm(true);
	msleep(1000);
	mvc3d_sys_write(SYS_MODE, FP_MODE);
	/* frame packing on: 3 s as always, up to 6 s for a slow mode set */
	for (i = 0; i < 6; i++) {
		msleep(1000);
		if (i >= 2 && READ_ONCE(mvc3d_fp_on))
			break;
	}
	if (!READ_ONCE(mvc3d_fp_on)) {
		pr_warn("display: frame packing did not come on\n");
		display_restore();
		r = -EIO;
		goto out;
	}
	/* UI planes off (HWC frames no longer switch them on), video layer on */
	for (i = 0; i < 3; i++)
		osd_set(i, "1", "1");
	mvc3d_sys_write(SYS_DISABLE, "0");
	mvc3d_sys_write(SYS_VGO, "1");
	{
		char v[32], a[32];

		mvc3d_sys_read(SYS_MODE, v, sizeof(v));
		mvc3d_sys_read(SYS_ATTR, a, sizeof(a));
		pr_info("display frame packed: mode %s attr %s\n", v, a);
	}
out:
	mutex_unlock(&disp_lock);
	return r;
}

/* (process context) the display as it was before mvc3d_display_take() */
void mvc3d_display_give_back(void)
{
	mutex_lock(&disp_lock);
	if (disp.taken)
		display_restore();
	mutex_unlock(&disp_lock);
}
