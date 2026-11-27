// SPDX-License-Identifier: GPL-2.0
/*
 * NZEUS: read-only probe of the HC16xx/E3100 AVP RPC mailbox.
 *
 * The factory AVP firmware (the second CPU's RTOS) owns the display output.
 * Stock Linux 4.4 talks to it with HiChip's GPL "amprpc" driver: an RPC
 * message in shared RAM, announced through a hardware mailbox, answered by
 * the AVP through a second mailbox. This driver reuses that message layout
 * and handshake (HiChip hichip-linux-drivers drivers/hcdrivers/amprpc and
 * include/uapi/hcuapi/amprpc.h, "Hi Chip Semiconductor", GPL) in a POLLED
 * form without the interrupt.
 *
 * Reading dis_query sends only fixed read queries to the AVP's /dev/dis
 * (DIS_GET_TVSYS and DIS_GET_SCREEN_INFO). Writing dis_action runs exactly one
 * of two named calls from the AVP's own display API, nothing else:
 *   resume_hd        DIS_SET_RESUME for the HD (panel) output
 *   reapply_tvsys_hd DIS_GET_TVSYS, then DIS_SET_TVSYS with the SAME values,
 *                    only if they are the observed 640x480 panel mode
 * Nothing touches the mailbox at boot. After one timeout every later call is
 * refused until the next boot.
 *
 * nzeus_avp_reapply_tvsys_hd() offers the same reapply call to the GMA
 * framebuffer driver, which uses it to recover from the AVP's periodic
 * output blank (about every 2 minutes) while Linux owns the screen.
 *
 * Brightness: a standard backlight device "r36s-avp" (0..255). The AVP owns
 * the backlight PWM; stock Linux only writes a 4-byte level to the AVP's
 * /dev/backlight through avp-proxy (RPC_OPEN, RPC_WRITE, RPC_CLOSE), and so
 * does this driver - nothing else, and nothing until a level is written. The
 * starting value is read (read-only) from the PWM channel 0 the AVP drives
 * (enable byte 0x18818414, counter 0x1881841c: high = duty ticks, low = rest;
 * the AVP sets duty = period * level / 255), e.g. high 24 + low 245 = level 23
 * as measured on 2026-10-10; 153 (factory default) if the PWM is off. It is
 * not tied to framebuffer blanking: our fb "blank" hands the screen back to
 * the firmware, it must not switch the backlight off.
 *
 * The upstream hc16xx intc driver does not reset its enable registers, and
 * it has no handler for the mailbox line here. If boot firmware left that
 * line enabled, waking the mailbox could cause an interrupt storm, so the
 * line's enable bit (intc ENABLE+8, bit 2) is read first and any query is
 * refused while it is set. This driver never writes the intc.
 */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/sysfs.h>
#include <asm/addrspace.h>
#include <asm/io.h>

/* Mailbox registers (amprpc mmbox_reg_struct.h): 0x20 bytes per mailbox. */
#define MBOX_TRIG		0x0
#define MBOX_CLR		0x4
#define MBOX_ENST		0x8
#define  ENST_REC_EN		BIT(0)
#define  ENST_CBK_EN		BIT(1)
#define  ENST_REC_STA		BIT(8)
#define  ENST_CBK_STA		BIT(16)
#define MBOX_DATA0		0xc
#define NMBOX			8
#define INTC_MBOX_ENABLE	BIT(2)	/* in the 17th reg region (intc ENABLE+8) */

/* hcuapi/amprpc.h */
#define RPC_CALL_M2S		1
#define RPC_RET_M2S		2
#define RPC_STATUS_SETUP	1
#define RPC_STATUS_FINISHED	4
#define RPC_OPEN		0x1000
#define RPC_CLOSE		0x1001
#define RPC_WRITE		0x1003
#define RPC_IOCTL		0x1004

/* hcuapi/dis.h, encoded as stock MIPS userspace sends them. */
#define DIS_IOCBASE		14
#define DIS_TVSYS_SIZE		16	/* 3 enums + bool, padded */
#define DIS_SCREEN_INFO_SIZE	12	/* enum + 4 x u16 */
#define DIS_GET_TVSYS		_IOC(_IOC_READ | _IOC_WRITE, DIS_IOCBASE, 1, DIS_TVSYS_SIZE)
#define DIS_GET_SCREEN_INFO	_IOC(_IOC_READ | _IOC_WRITE, DIS_IOCBASE, 12, DIS_SCREEN_INFO_SIZE)
#define DIS_SET_TVSYS		_IOC(_IOC_WRITE, DIS_IOCBASE, 0, DIS_TVSYS_SIZE)
#define DIS_SET_RESUME		_IOC(_IOC_WRITE, DIS_IOCBASE, 28, 4)
#define DIS_LAYER_MAIN		1
#define DIS_TYPE_HD		1
#define TV_LINE_800X480_60	15	/* HD tvtype observed on this console */
/* hcuapi/snd.h: SND_IOCTL_GET_HW_INFO = _IOR(SND_IOCBASE 8, 16, struct snd_hw_info 72 B) */
#define SND_GET_HW_INFO		_IOC(_IOC_READ, 8, 16, 72)
/* hcuapi/hdmi_tx.h (HDMI_TX_IOCBASE 28): EDID queries, read-only */
#define HDMI_GET_EDID_TVSYS	_IOC(_IOC_READ, 28, 0, 4)
#define HDMI_GET_EDIDINFO	_IOC(_IOC_READ, 28, 1, 76)
#define LINUX_O_RDWR		2	/* stock avp-proxy passes Linux f_flags */
#define BACKLIGHT_DEFAULT	153	/* /hcrtos/backlight default-brightness-level */
#define BACKLIGHT_MAX		255	/* /hcrtos/backlight scale */
#define PWM0_REGS		0x18818414	/* channel bytes, then counters at +8 */

#define POLL_MS			2000

struct rpc_hdr {
	u32 dir;
	u32 id;
	u32 status;
	u32 priv;
};

struct rpc_open {
	struct rpc_hdr rpc;
	s32 ret;
	s32 flags;
	s32 mode;
	char buf[];
};

struct rpc_close {
	struct rpc_hdr rpc;
	s32 ret;
	s32 fd;
};

struct rpc_write {
	struct rpc_hdr rpc;
	s32 ret;
	s32 fd;
	u32 nbytes;
	u8 buf[];
};

struct rpc_ioctl {
	struct rpc_hdr rpc;
	s32 ret;
	s32 fd;
	u32 cmd;
	u32 arg;
	u32 parg[];
};

struct avp_rpc {
	struct device *dev;
	void __iomem *mm[NMBOX];	/* our side: 0..3 calls, 4..7 replies */
	void __iomem *sm[NMBOX];	/* AVP side: reply data registers */
	void __iomem *intc_en;		/* read-only: intc mailbox enable word */
	void *msg;			/* uncached KSEG1 alias of one page */
	struct mutex lock;
	bool enabled;
	bool stuck;
	unsigned int foreign;		/* AVP messages that were not our reply */
	char last[256];			/* result of the last dis_action */
};

static struct avp_rpc *avp_rpc_instance;	/* single device; set at probe */

/* Enable the mailbox status bits exactly as stock amprpc_mbox_init() does. */
static void avp_rpc_enable(struct avp_rpc *p)
{
	int i;

	for (i = 0; i < 4; i++)
		writel(ENST_CBK_EN | ENST_CBK_STA, p->mm[i] + MBOX_ENST);
	for (i = 4; i < NMBOX; i++)
		writel(ENST_REC_EN | ENST_REC_STA, p->mm[i] + MBOX_ENST);
	p->enabled = true;
}

/* Post the message already built in p->msg and poll for the AVP's answer. */
static int avp_rpc_call(struct avp_rpc *p, s64 *ms)
{
	struct rpc_hdr *h = p->msg;
	u32 addr = (u32)(unsigned long)p->msg;
	ktime_t start = ktime_get();
	bool replied = false;
	u32 v, m;
	int j;

	/* A late "mailbox released" acknowledgement from the previous call. */
	v = readl(p->mm[0] + MBOX_ENST);
	if (v & ENST_CBK_STA)
		writel((v & ~ENST_REC_STA) | ENST_CBK_STA, p->mm[0] + MBOX_ENST);

	WRITE_ONCE(h->priv, 0);
	WRITE_ONCE(h->dir, RPC_CALL_M2S);
	WRITE_ONCE(h->status, RPC_STATUS_SETUP);
	wmb();
	writel(addr, p->mm[0] + MBOX_DATA0);
	if (readl(p->mm[0] + MBOX_DATA0) != addr)
		return -EIO;
	writel(1, p->mm[0] + MBOX_TRIG);

	while (ktime_ms_delta(ktime_get(), start) < POLL_MS) {
		v = readl(p->mm[0] + MBOX_ENST);
		if (v & ENST_CBK_STA)
			writel((v & ~ENST_REC_STA) | ENST_CBK_STA, p->mm[0] + MBOX_ENST);
		/* Same acknowledgement order as the stock interrupt handler. */
		for (j = 4; j < NMBOX; j++) {
			v = readl(p->mm[j] + MBOX_ENST);
			if (!(v & ENST_REC_STA))
				continue;
			m = readl(p->sm[j] + MBOX_DATA0);
			writel(0, p->sm[j] + MBOX_DATA0);
			writel((v & ~ENST_CBK_STA) | ENST_REC_STA, p->mm[j] + MBOX_ENST);
			writel(1, p->mm[j] + MBOX_CLR);
			if (m == addr)
				replied = true;
			else
				p->foreign++;
		}
		if (replied && READ_ONCE(h->dir) == RPC_RET_M2S)
			break;
		usleep_range(200, 500);
	}
	*ms = ktime_ms_delta(ktime_get(), start);
	WRITE_ONCE(h->status, RPC_STATUS_FINISHED);
	if (!replied || READ_ONCE(h->dir) != RPC_RET_M2S) {
		p->stuck = true;
		return -ETIMEDOUT;
	}
	return 0;
}

static int avp_rpc_ioctl(struct avp_rpc *p, int fd, u32 cmd, const u32 *in,
			 unsigned int words, u32 *out, s32 *ret, s64 *ms)
{
	struct rpc_ioctl *io = p->msg;
	unsigned int i;
	int err;

	memset(p->msg, 0, PAGE_SIZE);
	io->rpc.id = RPC_IOCTL;
	io->fd = fd;
	io->cmd = cmd;
	for (i = 0; i < words; i++)
		io->parg[i] = in[i];
	err = avp_rpc_call(p, ms);
	if (err)
		return err;
	*ret = READ_ONCE(io->ret);
	for (i = 0; i < words; i++)
		out[i] = READ_ONCE(io->parg[i]);
	return 0;
}

static ssize_t dis_query_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct avp_rpc *p = dev_get_drvdata(dev);
	struct rpc_open *op;
	struct rpc_close *cl;
	u32 in[4], out[4];
	s32 fd, ret;
	s64 ms;
	int len = 0, err, type;

	mutex_lock(&p->lock);
	if (p->stuck) {
		len = sysfs_emit(buf, "REFUSED_AFTER_EARLIER_TIMEOUT\n");
		goto out;
	}
	if (readl(p->intc_en) & INTC_MBOX_ENABLE) {
		len = sysfs_emit(buf, "REFUSED_MBOX_IRQ_LINE_ENABLED intc_en=%08x\n",
				 readl(p->intc_en));
		goto out;
	}
	if (!p->enabled)
		avp_rpc_enable(p);
	len += sysfs_emit_at(buf, len, "INTC_MBOX_ENABLE_WORD=%08x\n", readl(p->intc_en));

	memset(p->msg, 0, PAGE_SIZE);
	op = p->msg;
	op->rpc.id = RPC_OPEN;
	op->flags = 0;	/* O_RDONLY */
	op->mode = 0;
	strscpy(op->buf, "/dev/dis", 64);
	err = avp_rpc_call(p, &ms);
	if (err) {
		len = sysfs_emit(buf, "OPEN_FAILED err=%d ms=%lld foreign=%u\n",
				 err, ms, p->foreign);
		goto out;
	}
	fd = READ_ONCE(op->ret);
	len += sysfs_emit_at(buf, len, "OPEN /dev/dis fd=%d ms=%lld\n", fd, ms);
	if (fd < 0)
		goto done;

	for (type = 0; type <= 1; type++) {	/* DIS_TYPE_SD, DIS_TYPE_HD */
		memset(in, 0, sizeof(in));
		in[0] = type;
		in[1] = DIS_LAYER_MAIN;
		err = avp_rpc_ioctl(p, fd, DIS_GET_TVSYS, in, 4, out, &ret, &ms);
		if (err) {
			len += sysfs_emit_at(buf, len, "TVSYS type=%d FAILED err=%d\n", type, err);
			goto done;
		}
		len += sysfs_emit_at(buf, len,
			"TVSYS type=%d ret=%d distype=%u layer=%u tvtype=%u progressive=%u ms=%lld\n",
			type, ret, out[0], out[1], out[2], out[3] & 0xff, ms);
	}

	for (type = 0; type <= 1; type++) {
		memset(in, 0, sizeof(in));
		in[0] = type;
		err = avp_rpc_ioctl(p, fd, DIS_GET_SCREEN_INFO, in, 3, out, &ret, &ms);
		if (err) {
			len += sysfs_emit_at(buf, len, "SCREEN type=%d FAILED err=%d\n", type, err);
			goto done;
		}
		/* struct dis_area: x, y, w, h as u16, little-endian */
		len += sysfs_emit_at(buf, len,
			"SCREEN type=%d ret=%d distype=%u x=%u y=%u w=%u h=%u ms=%lld\n",
			type, ret, out[0], out[1] & 0xffff, out[1] >> 16,
			out[2] & 0xffff, out[2] >> 16, ms);
	}

	memset(p->msg, 0, PAGE_SIZE);
	cl = p->msg;
	cl->rpc.id = RPC_CLOSE;
	cl->fd = fd;
	err = avp_rpc_call(p, &ms);
	len += sysfs_emit_at(buf, len, "CLOSE err=%d ret=%d ms=%lld\n",
			     err, err ? 0 : READ_ONCE(cl->ret), ms);
done:
	len += sysfs_emit_at(buf, len, "FOREIGN_MESSAGES=%u\n", p->foreign);
out:
	mutex_unlock(&p->lock);
	return len;
}
static DEVICE_ATTR_RO(dis_query);

/* Common refusals; enables the mailbox on first use. Caller holds the lock. */
static const char *avp_rpc_ready(struct avp_rpc *p)
{
	if (p->stuck)
		return "REFUSED_AFTER_EARLIER_TIMEOUT";
	if (readl(p->intc_en) & INTC_MBOX_ENABLE)
		return "REFUSED_MBOX_IRQ_LINE_ENABLED";
	if (!p->enabled)
		avp_rpc_enable(p);
	return NULL;
}

static int avp_rpc_open_path(struct avp_rpc *p, const char *path, s32 *fd, s64 *ms)
{
	struct rpc_open *op = p->msg;
	int err;

	memset(p->msg, 0, PAGE_SIZE);
	op->rpc.id = RPC_OPEN;
	op->flags = LINUX_O_RDWR;
	strscpy(op->buf, path, 64);
	err = avp_rpc_call(p, ms);
	if (!err)
		*fd = READ_ONCE(op->ret);
	return err;
}

static int avp_rpc_open_dis(struct avp_rpc *p, s32 *fd, s64 *ms)
{
	return avp_rpc_open_path(p, "/dev/dis", fd, ms);
}

static int avp_rpc_close_dis(struct avp_rpc *p, s32 fd, s64 *ms)
{
	struct rpc_close *cl = p->msg;

	memset(p->msg, 0, PAGE_SIZE);
	cl->rpc.id = RPC_CLOSE;
	cl->fd = fd;
	return avp_rpc_call(p, ms);
}

/*
 * DIS_GET_TVSYS then DIS_SET_TVSYS with the same values, only for the
 * observed panel mode. Caller holds the lock and has passed avp_rpc_ready().
 */
static int avp_rpc_reapply_locked(struct avp_rpc *p)
{
	u32 in[4], out[4];
	s32 fd = -1, ret = 0, ret2 = 0;
	s64 ms;
	int err;

	err = avp_rpc_open_dis(p, &fd, &ms);
	if (err || fd < 0) {
		snprintf(p->last, sizeof(p->last), "OPEN_FAILED err=%d fd=%d", err, fd);
		return err ? err : -EIO;
	}
	memset(in, 0, sizeof(in));
	in[0] = DIS_TYPE_HD;
	in[1] = DIS_LAYER_MAIN;
	err = avp_rpc_ioctl(p, fd, DIS_GET_TVSYS, in, 4, out, &ret, &ms);
	if (err || ret || out[0] != DIS_TYPE_HD || out[1] != DIS_LAYER_MAIN ||
	    out[2] != TV_LINE_800X480_60 || (out[3] & 0xff) != 1) {
		snprintf(p->last, sizeof(p->last),
			 "REAPPLY_REFUSED_UNEXPECTED_TVSYS err=%d ret=%d words=%08x %08x %08x %08x",
			 err, ret, out[0], out[1], out[2], out[3]);
		err = err ? err : -EINVAL;
	} else {
		/* Set back exactly what the AVP just reported. */
		err = avp_rpc_ioctl(p, fd, DIS_SET_TVSYS, out, 4, in, &ret2, &ms);
		snprintf(p->last, sizeof(p->last),
			 "REAPPLY_TVSYS_HD err=%d ret=%d tvtype=%u progressive=%u ms=%lld",
			 err, ret2, out[2], out[3] & 0xff, ms);
		if (!err && ret2)
			err = -EIO;
	}
	if (!p->stuck)
		avp_rpc_close_dis(p, fd, &ms);
	return err;
}

/* For the GMA framebuffer driver (built in). Sleeps; never call in atomic context. */
int nzeus_avp_reapply_tvsys_hd(void);
int nzeus_avp_reapply_tvsys_hd(void)
{
	struct avp_rpc *p = READ_ONCE(avp_rpc_instance);
	const char *refusal;
	int err;

	if (!p)
		return -ENODEV;
	mutex_lock(&p->lock);
	refusal = avp_rpc_ready(p);
	if (refusal) {
		strscpy(p->last, refusal, sizeof(p->last));
		err = -EPERM;
	} else {
		err = avp_rpc_reapply_locked(p);
	}
	dev_info(p->dev, "auto reapply: %s\n", p->last);
	mutex_unlock(&p->lock);
	return err;
}

static ssize_t dis_action_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct avp_rpc *p = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&p->lock);
	len = sysfs_emit(buf, "%s\n", p->last[0] ? p->last : "NO_ACTION_YET");
	mutex_unlock(&p->lock);
	return len;
}

static ssize_t dis_action_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct avp_rpc *p = dev_get_drvdata(dev);
	bool resume = sysfs_streq(buf, "resume_hd");
	bool reapply = sysfs_streq(buf, "reapply_tvsys_hd");
	const char *refusal;
	u32 in[4], out[4];
	s32 fd = -1, ret = 0;
	s64 ms;
	int err;

	if (!resume && !reapply)
		return -EINVAL;
	mutex_lock(&p->lock);
	refusal = avp_rpc_ready(p);
	if (refusal) {
		strscpy(p->last, refusal, sizeof(p->last));
		goto out;
	}
	if (reapply) {
		avp_rpc_reapply_locked(p);
		goto out;
	}
	err = avp_rpc_open_dis(p, &fd, &ms);
	if (err || fd < 0) {
		snprintf(p->last, sizeof(p->last), "OPEN_FAILED err=%d fd=%d", err, fd);
		goto out;
	}
	memset(in, 0, sizeof(in));
	in[0] = DIS_TYPE_HD;
	err = avp_rpc_ioctl(p, fd, DIS_SET_RESUME, in, 1, out, &ret, &ms);
	snprintf(p->last, sizeof(p->last), "RESUME_HD err=%d ret=%d ms=%lld",
		 err, ret, ms);
	if (!p->stuck)
		avp_rpc_close_dis(p, fd, &ms);
out:
	dev_info(dev, "dis_action: %s\n", p->last);
	mutex_unlock(&p->lock);
	return count;
}
static DEVICE_ATTR_ADMIN_RW(dis_action);

#if IS_REACHABLE(CONFIG_BACKLIGHT_CLASS_DEVICE)
/* Exactly what stock avp-proxy does for write(/dev/backlight, &level, 4). */
static int avp_rpc_backlight_update(struct backlight_device *bd)
{
	struct avp_rpc *p = bl_get_data(bd);
	u32 level = backlight_get_brightness(bd);
	struct rpc_write *wr = p->msg;
	const char *refusal;
	s32 fd = -1, ret = 0;
	s64 ms;
	int err;

	mutex_lock(&p->lock);
	refusal = avp_rpc_ready(p);
	if (refusal) {
		strscpy(p->last, refusal, sizeof(p->last));
		err = -EPERM;
		goto out;
	}
	err = avp_rpc_open_path(p, "/dev/backlight", &fd, &ms);
	if (err || fd < 0) {
		snprintf(p->last, sizeof(p->last), "BACKLIGHT_OPEN_FAILED err=%d fd=%d", err, fd);
		err = err ? err : -EIO;
		goto out;
	}
	memset(p->msg, 0, PAGE_SIZE);
	wr->rpc.id = RPC_WRITE;
	wr->fd = fd;
	wr->nbytes = 4;
	put_unaligned_le32(level, wr->buf);
	err = avp_rpc_call(p, &ms);
	if (!err)
		ret = READ_ONCE(wr->ret);
	snprintf(p->last, sizeof(p->last), "BACKLIGHT level=%u err=%d ret=%d ms=%lld",
		 level, err, ret, ms);
	if (!err && ret != 4)
		err = -EIO;
	if (!p->stuck)
		avp_rpc_close_dis(p, fd, &ms);
out:
	dev_info(p->dev, "%s\n", p->last);
	mutex_unlock(&p->lock);
	return err;
}

static bool avp_rpc_backlight_controls_device(struct backlight_device *bd,
					      struct device *display_dev)
{
	return false;	/* never follow fb blank: that only hands the screen back */
}

static const struct backlight_ops avp_rpc_backlight_ops = {
	.update_status = avp_rpc_backlight_update,
	.controls_device = avp_rpc_backlight_controls_device,
};

/* The AVP's current level, from its PWM channel 0 (read-only). */
static int avp_rpc_backlight_level_now(struct avp_rpc *p)
{
	void __iomem *pwm = ioremap(PWM0_REGS, 12);
	u32 counter, high, low;
	u8 chan0;

	if (!pwm)
		return BACKLIGHT_DEFAULT;
	chan0 = readb(pwm);
	counter = readl(pwm + 8);
	iounmap(pwm);
	high = counter >> 16;
	low = counter & 0xffff;
	dev_info(p->dev, "backlight PWM0 enable=%u high=%u low=%u\n", chan0 >> 7, high, low);
	if (!(chan0 & BIT(7)) || !(high + low))
		return BACKLIGHT_DEFAULT;
	return DIV_ROUND_CLOSEST(high * BACKLIGHT_MAX, high + low);
}

static int avp_rpc_backlight_init(struct avp_rpc *p)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_FIRMWARE,
		.max_brightness = BACKLIGHT_MAX,
		.brightness = avp_rpc_backlight_level_now(p),
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};
	struct backlight_device *bd;

	/* Registration sends nothing; only a later brightness write does. */
	bd = devm_backlight_device_register(p->dev, "r36s-avp", p->dev, p,
					    &avp_rpc_backlight_ops, &props);
	return PTR_ERR_OR_ZERO(bd);
}
#else
static int avp_rpc_backlight_init(struct avp_rpc *p) { return 0; }
#endif

/*
 * Read-only sound query (purelinux/AUDIO.md): open the AVP's /dev/sndC0i2so
 * and ask SND_IOCTL_GET_HW_INFO, which returns the playback DMA ring address
 * and size, the PCM parameters and the MMIO addresses of the ring index
 * registers - i.e. where the audio hardware lives. Nothing is configured.
 */
static ssize_t snd_query_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct avp_rpc *p = dev_get_drvdata(dev);
	u32 in[18] = { 0 }, out[18];
	const char *refusal;
	s32 fd = -1, ret = 0;
	ssize_t len = 0;
	s64 ms;
	int err, i;

	mutex_lock(&p->lock);
	refusal = avp_rpc_ready(p);
	if (refusal) {
		len = sysfs_emit(buf, "%s\n", refusal);
		goto out;
	}
	err = avp_rpc_open_path(p, "/dev/sndC0i2so", &fd, &ms);
	if (err || fd < 0) {
		len = sysfs_emit(buf, "OPEN_FAILED err=%d fd=%d\n", err, fd);
		goto out;
	}
	err = avp_rpc_ioctl(p, fd, SND_GET_HW_INFO, in, 18, out, &ret, &ms);
	len = sysfs_emit(buf, "SND_GET_HW_INFO err=%d ret=%d ms=%lld\n", err, ret, ms);
	if (!err)
		for (i = 0; i < 18; i++)
			len += sysfs_emit_at(buf, len, "W%02d=%08x%s", i, out[i], i % 6 == 5 ? "\n" : " ");
	len += sysfs_emit_at(buf, len, "(words: dma_addr dma_size, 56-byte pcm_params, wr_idx_reg rd_idx_reg)\n");
	if (!p->stuck)
		avp_rpc_close_dis(p, fd, &ms);
out:
	mutex_unlock(&p->lock);
	return len;
}
static DEVICE_ATTR_ADMIN_RO(snd_query);

/*
 * Read-only HDMI query (purelinux/BT-HDMI.md): open the AVP's /dev/hdmi
 * read-only and ask for the connected monitor's EDID (best mode and mode
 * list). Errors simply mean no cable/EDID. Nothing is switched.
 */
static ssize_t hdmi_query_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct avp_rpc *p = dev_get_drvdata(dev);
	struct rpc_open *op = p->msg;
	u32 in[19] = { 0 }, out[19];
	const char *refusal;
	s32 fd = -1, ret = 0;
	ssize_t len = 0;
	s64 ms;
	int err, i;

	mutex_lock(&p->lock);
	refusal = avp_rpc_ready(p);
	if (refusal) {
		len = sysfs_emit(buf, "%s\n", refusal);
		goto out;
	}
	memset(p->msg, 0, PAGE_SIZE);
	op->rpc.id = RPC_OPEN;
	op->flags = 0;			/* O_RDONLY, like stock cubevol */
	strscpy(op->buf, "/dev/hdmi", 64);
	err = avp_rpc_call(p, &ms);
	if (!err)
		fd = READ_ONCE(op->ret);
	if (err || fd < 0) {
		len = sysfs_emit(buf, "OPEN_FAILED err=%d fd=%d\n", err, fd);
		goto out;
	}
	err = avp_rpc_ioctl(p, fd, HDMI_GET_EDID_TVSYS, in, 1, out, &ret, &ms);
	len = sysfs_emit(buf, "EDID_TVSYS err=%d ret=%d tvsys=%u\n", err, ret, out[0]);
	if (!err && !p->stuck) {
		err = avp_rpc_ioctl(p, fd, HDMI_GET_EDIDINFO, in, 19, out, &ret, &ms);
		len += sysfs_emit_at(buf, len, "EDIDINFO err=%d ret=%d\n", err, ret);
		if (!err)
			for (i = 0; i < 19; i++)
				len += sysfs_emit_at(buf, len, "W%02d=%08x%s", i, out[i], i % 6 == 5 ? "\n" : " ");
		len += sysfs_emit_at(buf, len, "\n(tvsys[16], num_tvsys, audfmt, best_tvsys)\n");
	}
	if (!p->stuck)
		avp_rpc_close_dis(p, fd, &ms);
out:
	mutex_unlock(&p->lock);
	return len;
}
static DEVICE_ATTR_ADMIN_RO(hdmi_query);

static struct attribute *avp_rpc_attrs[] = {
	&dev_attr_dis_query.attr,
	&dev_attr_snd_query.attr,
	&dev_attr_hdmi_query.attr,
	&dev_attr_dis_action.attr,
	NULL,
};
ATTRIBUTE_GROUPS(avp_rpc);

static int avp_rpc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct avp_rpc *p;
	struct resource *res;
	unsigned long page;
	int i;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	for (i = 0; i < NMBOX; i++) {
		p->mm[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(p->mm[i]))
			return PTR_ERR(p->mm[i]);
		p->sm[i] = devm_platform_ioremap_resource(pdev, NMBOX + i);
		if (IS_ERR(p->sm[i]))
			return PTR_ERR(p->sm[i]);
	}
	/* Shared with the intc driver, which maps it without requesting it. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 2 * NMBOX);
	if (!res)
		return -EINVAL;
	p->intc_en = devm_ioremap(dev, res->start, resource_size(res));
	if (!p->intc_en)
		return -ENOMEM;
	/*
	 * One whole page, written back and invalidated, then used only through
	 * its uncached alias, as stock amprpc does. Never freed: the AVP may
	 * still write into it after a timeout.
	 */
	page = __get_free_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return -ENOMEM;
	dma_cache_wback_inv(page, PAGE_SIZE);
	p->msg = (void *)CKSEG1ADDR(page);
	mutex_init(&p->lock);
	platform_set_drvdata(pdev, p);
	WRITE_ONCE(avp_rpc_instance, p);
	i = avp_rpc_backlight_init(p);
	if (i)
		dev_warn(dev, "backlight device not registered: %d\n", i);
	dev_info(dev, "AVP RPC mailbox mapped; read-only queries only on demand\n");
	return 0;
}

static const struct of_device_id avp_rpc_of_match[] = {
	{ .compatible = "nzeus,hc16xx-avp-rpc-poll" },
	{ }
};

static struct platform_driver avp_rpc_driver = {
	.probe = avp_rpc_probe,
	.driver = {
		.name = "nzeus-avp-rpc",
		.of_match_table = avp_rpc_of_match,
		.dev_groups = avp_rpc_groups,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(avp_rpc_driver);
