/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _DRM_HAIKU_H
#define _DRM_HAIKU_H

/*	Haiku additions to the DRM device interface, for libdrm on Haiku.

	Haiku cannot mmap() a device, so DRM_HAIKU_IOCTL_MAP maps a buffer
	object at the fake offset the driver's GEM_MMAP ioctl returned into the
	calling team, as Linux's mmap() of the DRM file would, and
	DRM_HAIKU_IOCTL_UNMAP undoes it. The area belongs to the team; deleting
	it without UNMAP keeps the buffer referenced until the file is closed. */

#if defined(__KERNEL__) || defined(_KERNEL_MODE)
#	ifndef __user
#		define __user
#	endif
#endif

struct drm_haiku_map {
	__u64 offset;		/* in: fake offset from GEM_MMAP */
	__u64 size;			/* in: bytes, page aligned */
	__u32 flags;		/* in: DRM_HAIKU_MAP_WRITE */
	__s32 area;			/* out: Haiku area_id */
	__u64 address;		/* in: wanted address or 0; out: the address */
};

#define DRM_HAIKU_MAP_WRITE	0x1
#define DRM_HAIKU_MAP_CLONEABLE	0x2	/* other teams may clone the area */

struct drm_haiku_unmap {
	__u64 address;		/* in: address DRM_HAIKU_IOCTL_MAP returned */
};

/*	What Linux's libdrm reads from sysfs and stat(): the node and the PCI
	device behind a DRM file. */
struct drm_haiku_device_info {
	__u32 minor;			/* 0.. card, 128.. render */
	__u32 node_type;		/* 0 primary, 2 render (libdrm's DRM_NODE_*) */
	__u16 domain;
	__u8 bus;
	__u8 dev;
	__u8 func;
	__u8 revision_id;
	__u16 vendor_id;
	__u16 device_id;
	__u16 subvendor_id;
	__u16 subdevice_id;
	__u32 class_code;		/* base << 16 | sub << 8 | interface */
	__u32 file_id[2];		/* low, high: the same for two descriptors of the
							   same open file (one dup()ed from the other) */
	__u32 display_lost;		/* how often the display lost what it showed
							   (a suspend to RAM): it has to be drawn again */
	__u32 reserved;
};

/* 'd' 0xf0..0xff are free in the DRM core range */
#define DRM_HAIKU_IOCTL_MAP		DRM_IOWR(0xf0, struct drm_haiku_map)
#define DRM_HAIKU_IOCTL_UNMAP	DRM_IOW(0xf1, struct drm_haiku_unmap)
#define DRM_HAIKU_IOCTL_DEVICE_INFO	DRM_IOR(0xf2, struct drm_haiku_device_info)

/*	Hot plugging: Linux sends a uevent, Haiku has this. Waits until a
	monitor is attached or detached after \a generation (as returned by an
	earlier call, 0 the first time) or \a timeout_ms have passed; returns
	the current generation. */
struct drm_haiku_wait_hotplug {
	__u32 generation;	/* in: the last one seen; out: the current one */
	__u32 timeout_ms;	/* in */
};

#define DRM_HAIKU_IOCTL_WAIT_HOTPLUG	DRM_IOWR(0xf3, struct drm_haiku_wait_hotplug)

/*	What writing "detect", "off" or "on" to a connector's status in sysfs does
	on Linux: forces the connector disconnected or connected, or lets the
	hardware say again; a change is a hot plug event. Primary nodes only. */
struct drm_haiku_force_connector {
	__u32 connector_id;
	__u32 force;		/* 0 detect, 1 off, 2 on */
};

#define DRM_HAIKU_IOCTL_FORCE_CONNECTOR	DRM_IOW(0xf4, struct drm_haiku_force_connector)

/*	What writing /sys/module/<module>/parameters/<name> does on Linux: sets
	one of the driver's module parameters (amdgpu's and drm's, as in its
	settings file), which counts where Linux looks at it again; drm's "debug"
	is. Primary nodes only. */
struct drm_haiku_set_param {
	char name[64];
	char value[64];
};

#define DRM_HAIKU_IOCTL_SET_PARAM	DRM_IOW(0xf5, struct drm_haiku_set_param)

/*	The built-in panel's backlight (what Linux has in /sys/class/backlight):
	sets \a brightness (0 to max_brightness) unless it is negative, and
	returns the level and the maximum. Primary nodes only; ENODEV without a
	panel backlight. */
struct drm_haiku_backlight {
	__s32 brightness;		/* in: the new level, or -1; out: the level */
	__u32 max_brightness;	/* out */
};

#define DRM_HAIKU_IOCTL_BACKLIGHT	DRM_IOWR(0xf6, struct drm_haiku_backlight)

/* The CRC the display controller computes over each frame it scans out of
   a CRTC (what Linux offers through debugfs crtc-N/crc): with op 1 the
   CRTC starts computing it for every frame, op 2 reads the last complete
   frame's (three values: R/Cr, G/Y, B/Cb) with the vblank count, op 0
   stops it. A torn frame has a CRC that is neither picture's. Primary
   nodes only. */
struct drm_haiku_crc {
	__u32 crtc_index;		/* in */
	__u32 op;				/* in: 0 stop, 1 start, 2 read */
	__u32 frame;			/* out: the vblank count */
	__u32 crc[3];			/* out */
	__u16 x0, y0, x1, y1;	/* in, op 1: the window of the monitor's */
							/* pixels the CRC covers (inclusive); all */
							/* zero: the whole frame */
};

#define DRM_HAIKU_IOCTL_CRC		DRM_IOWR(0xf7, struct drm_haiku_crc)

/*	The screen's frame buffer, for a renderer that copies its finished frames
	straight into it (Vulkan: VkImportScanoutMemoryHAIKU) instead of handing
	them to app_server. The accelerant publishes the buffer it scans out
	from whenever that changes (op 1, primary nodes only: its handle and
	shape) and withdraws it (op 0); any client of the GPU then gets a handle
	to it on its own file (op 2), with the shape, the size and a generation
	that changes with every publish. The published buffer stays referenced
	until it is replaced or withdrawn, or the publisher's file closes. */
struct drm_haiku_scanout {
	__u32 op;			/* in: 0 withdraw, 1 publish, 2 get */
	__u32 handle;		/* in (1): the buffer; out (2): a handle on this file */
	__u32 width;		/* in (1), out (2): pixels */
	__u32 height;
	__u32 pitch;		/* in (1), out (2): bytes per row */
	__u32 format;		/* in (1), out (2): DRM_FORMAT_* */
	__u32 generation;	/* out */
	__u32 reserved;
	__u64 size;			/* out (2): bytes */
};

#define DRM_HAIKU_SCANOUT_WITHDRAW	0
#define DRM_HAIKU_SCANOUT_PUBLISH	1
#define DRM_HAIKU_SCANOUT_GET		2

#define DRM_HAIKU_IOCTL_SCANOUT	DRM_IOWR(0xf8, struct drm_haiku_scanout)

/* DRM_HAIKU_IOCTL_HOST_BO (nouveau_drm): a buffer object in GART made of the
 * caller's own memory (page aligned), which stays wired until the object is
 * freed. Several GPUs can each have one of the same memory: app_server's
 * frame buffer source, which every GPU of a desktop copies from. */
struct drm_haiku_host_bo {
	__u64 address;		/* in: page aligned */
	__u64 size;			/* in: page aligned */
	__u32 handle;		/* out: GEM handle on this file */
	__u32 flags;		/* in: 0 */
	__u64 offset;		/* out: the buffer's GPU address in this file's VM */
};

#define DRM_HAIKU_IOCTL_HOST_BO	DRM_IOWR(0xf9, struct drm_haiku_host_bo)

#endif /* _DRM_HAIKU_H */
