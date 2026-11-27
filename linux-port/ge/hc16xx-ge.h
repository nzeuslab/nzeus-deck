/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * HiChip HC16xx 2D graphics engine: user-space interface.
 *
 * mmap() maps one DMA-coherent command queue, and no more than that. It is
 * uncached, so words written by the CPU are visible to the engine at once
 * without cache maintenance. Pixel memory is not part of this mapping: the
 * engine reads pictures from the framebuffer, whose memory userspace already
 * has mapped and whose physical address comes from FBIOGET_FSCREENINFO.
 *
 * A queue is a list of 32-bit words consumed by the engine. Every word that
 * holds an address is physical: queue offsets use queue_phys, framebuffer
 * offsets use fix.smem_start. This driver does not interpret the words; the
 * players document them. Nothing checks those addresses, so open() needs
 * CAP_SYS_RAWIO.
 */
#ifndef _UAPI_LINUX_HC16XX_GE_H
#define _UAPI_LINUX_HC16XX_GE_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define HC16XX_GE_QUEUE_SIZE	0x10000

struct hc16xx_ge_info {
	__u32 version;		/* engine version register */
	__u32 queue_phys;	/* physical address of the mapped queue */
	__u32 queue_size;
};

/*
 * One queue, as byte offsets into the mapping: start is the first word, end
 * the last one (the engine's end-of-queue register takes the address of the
 * last word, not one past it). Both must be 4-byte aligned, and end must not
 * exceed queue_size - 4.
 */
struct hc16xx_ge_kick {
	__u32 start;
	__u32 end;
};

#define HC16XX_GE_IOC_MAGIC	'g'
#define HC16XX_GE_GET_INFO	_IOR(HC16XX_GE_IOC_MAGIC, 0x10, struct hc16xx_ge_info)
#define HC16XX_GE_KICK		_IOW(HC16XX_GE_IOC_MAGIC, 0x11, struct hc16xx_ge_kick)
#define HC16XX_GE_WAIT		_IO(HC16XX_GE_IOC_MAGIC, 0x12)
#define HC16XX_GE_RESET		_IO(HC16XX_GE_IOC_MAGIC, 0x13)

#endif /* _UAPI_LINUX_HC16XX_GE_H */
