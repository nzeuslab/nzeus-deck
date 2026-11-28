/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * HiChip HC16xx 2D graphics engine: user-space interface.
 *
 * mmap() at offset 0 maps one DMA-coherent command queue, and no more than
 * that. It is uncached, so words written by the CPU are visible to the engine
 * at once without cache maintenance. The engine reads pictures from the
 * framebuffer, whose memory userspace already has mapped and whose physical
 * address comes from FBIOGET_FSCREENINFO, or from the picture buffer below.
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

/*
 * The picture buffer: physically contiguous memory that the CPU maps cached,
 * so that an emulator can draw its frames there and the engine can read them
 * without a copy. It is mapped at this offset of the device; HC16XX_GE_BUFFER
 * allocates it first. Since the CPU caches it, HC16XX_GE_SYNC must write a
 * range back before the engine reads it. The buffer is freed when the device
 * is closed and no longer mapped.
 */
#define HC16XX_GE_BUF_OFFSET	0x10000
#define HC16XX_GE_BUF_SIZE	0x100000

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

struct hc16xx_ge_buffer {
	__u32 phys;		/* physical address of the picture buffer */
	__u32 size;
};

/* A byte range of the picture buffer, written back from the CPU caches. */
struct hc16xx_ge_sync {
	__u32 offset;
	__u32 size;
};

#define HC16XX_GE_IOC_MAGIC	'g'
#define HC16XX_GE_GET_INFO	_IOR(HC16XX_GE_IOC_MAGIC, 0x10, struct hc16xx_ge_info)
#define HC16XX_GE_KICK		_IOW(HC16XX_GE_IOC_MAGIC, 0x11, struct hc16xx_ge_kick)
#define HC16XX_GE_WAIT		_IO(HC16XX_GE_IOC_MAGIC, 0x12)
#define HC16XX_GE_RESET		_IO(HC16XX_GE_IOC_MAGIC, 0x13)
#define HC16XX_GE_BUFFER	_IOR(HC16XX_GE_IOC_MAGIC, 0x14, struct hc16xx_ge_buffer)
#define HC16XX_GE_SYNC		_IOW(HC16XX_GE_IOC_MAGIC, 0x15, struct hc16xx_ge_sync)

#endif /* _UAPI_LINUX_HC16XX_GE_H */
