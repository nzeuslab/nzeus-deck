// SPDX-License-Identifier: GPL-2.0
/*
 * NZEUS R36S sound card (pure Linux, no AVP): ALSA playback through the
 * SoC's I2S block SND0 (0x1880a000) to the TM8211 DAC and the 8002D speaker
 * amp. Card "R36S", one PCM (hw:0,0): S16_LE stereo, 44100 or 48000 Hz;
 * control "Master Playback Volume" 0-255 = the block's own volume byte.
 *
 * Hardware 2026-10-11 (the test version of this driver): with the factory
 * AVP's decoded set-up (ports/r36s-hichip/purelinux/AUDIO-AVP-DECODE.md) the DMA
 * read index moved at exactly 48 kHz, and a 375 Hz test tone was heard;
 * L19 LOW = amp on (proven).
 *
 * How playback works:
 *   - the ALSA buffer IS the hardware ring (+0x30 address, +0x34 size/16);
 *   - +0x38 = software write index, +0x3a = hardware read index, both in
 *     16-byte units; wr == rd means empty, so a FULL buffer is told as
 *     "full minus 16 bytes";
 *   - +0x04 BIT16 = GO; interrupts stay OFF (no handler on IRQ 5 yet): an
 *     hrtimer polls the read index and reports elapsed periods;
 *   - the amp (L19) is on only while a stream runs (no hiss, no battery use).
 * Probe touches NOTHING: the clocks, PLL and pins are set up at the first
 * prepare. System-controller registers are shared with display, USB and SD:
 * changed bit by bit (read-modify-write) only. SND0 is never read while its
 * clock is gated (could hang the bus).
 *
 * sysfs (root only): "regs" - the audio registers, for debugging.
 *
 * Headphones (2026-10-12): the stock DT's headphone-detect pin is L15 (input).
 * Which level means "plugged in" is NOT known yet, so the driver only reads
 * it; sysfs "headphones" (root) shows the raw level, and writing 0 or 1 says
 * which level means plugged ("off" = ignore the pin, the default). While
 * headphones are in, the speaker amp stays off during playback (the DAC
 * output still reaches the jack). No new kernel is needed to set it up.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#define SYS_BASE	0x18800000	/* system controller (shared: RMW only) */
#define SYS_SIZE	0x500

/* system controller offsets */
#define SYS_CHIPID	0x000
#define SYS_GATE1	0x064		/* bit set = clock OFF: 15 AUDIO, 20 SPO, 21 DDP_SPO */
#define SYS_I2S_SRC	0x06c		/* bits 4-7: I2S source = audio pads */
#define SYS_RESET1	0x080		/* bit 5: audio block reset */
#define SYS_PADCLK	0x08c		/* bits 16-26: audio pad clocks (set = off) */
#define SYS_EJTAG	0x094		/* bit 16: EJTAG off on L01-L05 */
#define SYS_I2SDIV	0x148		/* [31:23] MCLK div, [9:0] BCLK div */
#define SYS_I2SCTL	0x154		/* bit 14 */
#define SYS_APLL0	0x470		/* [19:16] = 1, [23:20] = 2, bit 3 = 0 */
#define SYS_APLL_F	0x474		/* [23:0] fraction */
#define SYS_APLL_M	0x478		/* u16 */
#define SYS_APLL_N	0x47a		/* u16 */
#define SYS_PINMUX(p)	(0x4a0 + (p))	/* port L pin p function byte */

/* SND0 offsets */
#define SND_CTRL	0x04		/* BIT0 | BIT8 setup, BIT16 = GO */
#define SND_IRQ_PERIOD	0x08		/* bit0 enable, upper bits status (W1C) */
#define SND_IRQ_UNDER	0x0c		/* bit0 enable, upper bits status (W1C) */
#define SND_RING	0x30		/* ring physical address */
#define SND_RINGCFG	0x34		/* [15:0] size/16, [17:16] ch, [21:20] width, 24/25/27 */
#define SND_WR		0x38		/* u16 software write index (16-byte units) */
#define SND_RD		0x3a		/* u16 hardware read index (16-byte units) */
#define SND_FMT		0x50		/* [1:0] format (lj = 1), [5:4], 10, 11 slave, 29 */
#define SND_PERIOD	0x5c		/* u16 period size (frames) */
#define SND_FADE	0x90		/* byte: bit0 fade on */
#define SND_FADESTEP	0x91		/* byte */
#define SND_VOL		0x92		/* byte: target volume */
#define SND_VOLNOW	0x93		/* byte: current volume */
#define SND_FADESTEP2	0x98		/* byte */

#define DEFAULT_VOLUME	64		/* of 255: about a quarter, the speaker is loud */
#define POLL_NS		(4 * NSEC_PER_MSEC)

struct r36s_audio {
	struct device *dev;
	void __iomem *sys, *snd;
	struct gpio_desc *mute;		/* L19: logical 1 = amp off */
	struct gpio_desc *hp;		/* L15 headphone detect (input), NULL if not described */
	int hp_plugged;			/* raw L15 level meaning "plugged in"; -1 = ignore the pin */
	bool amp_on;			/* what we last told L19 */
	struct mutex lock;		/* set-up vs. regs */
	struct snd_card *card;
	struct snd_pcm_substream *substream;
	struct hrtimer timer;
	bool running;
	unsigned int rate;		/* rate the PLL is set for, 0 = not set up */
	unsigned int volume;		/* 0-255 */
	unsigned int ring_bytes, period_bytes;
	unsigned int last_pos, elapsed;	/* bytes, for period reports */
};

/* headphones in? (only once I know which level means plugged) */
static bool hp_in(struct r36s_audio *a)
{
	return a->hp && a->hp_plugged >= 0 && gpiod_get_raw_value(a->hp) == a->hp_plugged;
}

/* the speaker amp: on only while playing and no headphones are in */
static void amp_set(struct r36s_audio *a, bool playing)
{
	bool on = playing && !hp_in(a);

	if (on != a->amp_on) {
		gpiod_set_value(a->mute, on ? 0 : 1);
		a->amp_on = on;
	}
}

static void sys_rmw(struct r36s_audio *a, unsigned int off, u32 clear, u32 set)
{
	u32 before = readl(a->sys + off), after = (before & ~clear) | set;

	if (after != before)
		writel(after, a->sys + off);
	dev_dbg(a->dev, "sys+0x%03x %08x -> %08x\n", off, before, after);
}

static void snd_rmw(struct r36s_audio *a, unsigned int off, u32 clear, u32 set)
{
	writel((readl(a->snd + off) & ~clear) | set, a->snd + off);
}

static bool audio_clock_on(struct r36s_audio *a)
{
	return !(readl(a->sys + SYS_GATE1) & BIT(15));
}

/* The factory AVP's own bring-up (AUDIO-AVP-DECODE.md section 7), for the
 * 48 kHz or the 44.1 kHz family. Not while a stream runs. */
static int r36s_audio_setup(struct r36s_audio *a, unsigned int rate)
{
	u32 id = readl(a->sys + SYS_CHIPID);
	int i;

	if ((id >> 16) == 0x1512) {
		dev_err(a->dev, "chip id %08x is HC15xx: the PLL values are for HC16xx\n", id);
		return -ENODEV;
	}
	gpiod_direction_output(a->mute, 1);			/* amp off first */
	a->amp_on = false;
	if (!a->rate) {						/* first time: reset, clocks, pins */
		sys_rmw(a, SYS_RESET1, 0, BIT(5));
		usleep_range(1000, 2000);
		sys_rmw(a, SYS_RESET1, BIT(5), 0);
		sys_rmw(a, SYS_GATE1, BIT(15) | BIT(20) | BIT(21), 0);
		sys_rmw(a, SYS_PADCLK, GENMASK(26, 16), 0);
		if (!(readl(a->sys + SYS_EJTAG) & BIT(16)))
			sys_rmw(a, SYS_EJTAG, 0, BIT(16));
	}
	/* audio PLL (non-HC15xx branch), stock order */
	writew(rate == 44100 ? 316 : 344, a->sys + SYS_APLL_N);
	writew(7, a->sys + SYS_APLL_M);
	sys_rmw(a, SYS_APLL0, GENMASK(23, 16) | BIT(3), (1 << 16) | (2 << 20));
	sys_rmw(a, SYS_APLL_F, GENMASK(23, 0), rate == 44100 ? 0x1b6000 : 0x106000);
	usleep_range(1000, 2000);
	if (!a->rate) {
		for (i = 3; i <= 5; i++)			/* L03 L04 I2S clocks, L05 data */
			writeb(2, a->sys + SYS_PINMUX(i));
		sys_rmw(a, SYS_I2S_SRC, GENMASK(7, 4), 0);
	}
	if (!audio_clock_on(a)) {
		dev_err(a->dev, "audio clock still gated: SND0 not touched\n");
		return -EIO;
	}
	/* 16-bit, 32 fs BCLK, master, left-justified; MCLK/BCLK dividers (same for both rates) */
	snd_rmw(a, SND_FMT, 0x30 | BIT(10) | BIT(11) | BIT(29), 0);
	sys_rmw(a, SYS_I2SDIV, GENMASK(31, 23), 16 << 23);
	sys_rmw(a, SYS_I2SDIV, GENMASK(9, 0), 192);
	sys_rmw(a, SYS_I2SCTL, 0, BIT(14));
	snd_rmw(a, SND_CTRL, BIT(16), BIT(0) | BIT(8));
	snd_rmw(a, SND_FMT, GENMASK(1, 0), 1);
	/* no fade, fade step 10, interrupts OFF (status bits are W1C: kept) */
	writeb(readb(a->snd + SND_FADE) & ~1, a->snd + SND_FADE);
	writeb(10, a->snd + SND_FADESTEP);
	writeb(10, a->snd + SND_FADESTEP2);
	snd_rmw(a, SND_IRQ_PERIOD, BIT(0) | BIT(8), 0);
	snd_rmw(a, SND_IRQ_UNDER, BIT(0), 0);
	if (a->rate != rate)
		dev_info(a->dev, "set up for %u Hz (chip id %08x)\n", rate, id);
	a->rate = rate;
	return 0;
}

/* +0x38 = where the application's data ends (a full ring = one unit less) */
static void r36s_audio_set_wr(struct r36s_audio *a, struct snd_pcm_runtime *rt)
{
	snd_pcm_uframes_t queued = snd_pcm_playback_hw_avail(rt);
	unsigned int bytes, units = a->ring_bytes / 16;

	if (queued >= rt->buffer_size)
		queued = rt->buffer_size - 4;			/* 4 frames = 16 bytes */
	bytes = frames_to_bytes(rt, (rt->status->hw_ptr + queued) % rt->buffer_size);
	writew((bytes / 16) % units, a->snd + SND_WR);
}

static enum hrtimer_restart r36s_audio_poll(struct hrtimer *t)
{
	struct r36s_audio *a = container_of(t, struct r36s_audio, timer);
	struct snd_pcm_substream *ss = a->substream;
	unsigned int pos, delta;
	unsigned long flags;
	bool period = false;

	if (!a->running || !ss)
		return HRTIMER_NORESTART;
	snd_pcm_stream_lock_irqsave(ss, flags);
	if (a->running) {
		pos = (readw(a->snd + SND_RD) * 16) % a->ring_bytes;
		delta = (pos + a->ring_bytes - a->last_pos) % a->ring_bytes;
		a->last_pos = pos;
		a->elapsed += delta;
		if (a->elapsed >= a->period_bytes) {
			a->elapsed %= a->period_bytes;
			period = true;
		}
		/* ran dry (the program stopped writing): tell ALSA now, so it sees the
		 * underrun and stops the stream (amp off) instead of waiting for the
		 * next call from the program (hardware 2026-10-11: stayed RUNNING) */
		if (!delta && !snd_pcm_playback_hw_avail(ss->runtime))
			period = true;
		r36s_audio_set_wr(a, ss->runtime);		/* (a full ring may have room again) */
		amp_set(a, true);				/* headphones plugged/unplugged meanwhile */
	}
	snd_pcm_stream_unlock_irqrestore(ss, flags);
	if (period)
		snd_pcm_period_elapsed(ss);
	hrtimer_forward_now(t, ns_to_ktime(POLL_NS));
	return a->running ? HRTIMER_RESTART : HRTIMER_NORESTART;
}

static const struct snd_pcm_hardware r36s_audio_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
	.rate_min = 44100,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 64 * 1024,
	.period_bytes_min = 512,
	.period_bytes_max = 16 * 1024,
	.periods_min = 2,
	.periods_max = 64,
};

static int r36s_audio_open(struct snd_pcm_substream *ss)
{
	struct r36s_audio *a = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	int ret;

	rt->hw = r36s_audio_hw;
	/* the ring is counted in 16-byte units and must start 32-byte aligned */
	ret = snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 32);
	if (!ret)
		ret = snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 32);
	if (!ret)
		ret = snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;
	a->substream = ss;
	return 0;
}

static int r36s_audio_close(struct snd_pcm_substream *ss)
{
	struct r36s_audio *a = snd_pcm_substream_chip(ss);

	hrtimer_cancel(&a->timer);
	amp_set(a, false);
	a->substream = NULL;
	return 0;
}

static int r36s_audio_prepare(struct snd_pcm_substream *ss)
{
	struct r36s_audio *a = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	int ret;

	mutex_lock(&a->lock);
	ret = r36s_audio_setup(a, rt->rate);
	mutex_unlock(&a->lock);
	if (ret)
		return ret;
	a->ring_bytes = snd_pcm_lib_buffer_bytes(ss);
	a->period_bytes = snd_pcm_lib_period_bytes(ss);
	a->last_pos = 0;
	a->elapsed = 0;
	writel(rt->dma_addr & 0x1fffffff, a->snd + SND_RING);
	writel(BIT(27) | BIT(25) | BIT(24) | (1 << 20) | (1 << 16) | (a->ring_bytes / 16),
	       a->snd + SND_RINGCFG);
	writew(rt->period_size, a->snd + SND_PERIOD);
	writew(0, a->snd + SND_WR);
	writeb(a->volume, a->snd + SND_VOL);
	return 0;
}

static int r36s_audio_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct r36s_audio *a = snd_pcm_substream_chip(ss);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		r36s_audio_set_wr(a, ss->runtime);
		snd_rmw(a, SND_CTRL, 0, BIT(16));		/* GO */
		amp_set(a, true);				/* amp on (not with headphones in) */
		a->running = true;
		hrtimer_start(&a->timer, ns_to_ktime(POLL_NS), HRTIMER_MODE_REL);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		a->running = false;
		amp_set(a, false);				/* amp off first (no pop) */
		snd_rmw(a, SND_CTRL, BIT(16), 0);
		hrtimer_try_to_cancel(&a->timer);
		return 0;
	}
	return -EINVAL;
}

/* the timer may still be in its last run after STOP: wait for it (not atomic here) */
static int r36s_audio_sync_stop(struct snd_pcm_substream *ss)
{
	struct r36s_audio *a = snd_pcm_substream_chip(ss);

	hrtimer_cancel(&a->timer);
	return 0;
}

static int r36s_audio_ack(struct snd_pcm_substream *ss)
{
	struct r36s_audio *a = snd_pcm_substream_chip(ss);

	if (a->running)
		r36s_audio_set_wr(a, ss->runtime);
	return 0;
}

static snd_pcm_uframes_t r36s_audio_pointer(struct snd_pcm_substream *ss)
{
	struct r36s_audio *a = snd_pcm_substream_chip(ss);
	unsigned int pos = (readw(a->snd + SND_RD) * 16) % a->ring_bytes;

	return bytes_to_frames(ss->runtime, pos);
}

static const struct snd_pcm_ops r36s_audio_ops = {
	.open = r36s_audio_open,
	.close = r36s_audio_close,
	.prepare = r36s_audio_prepare,
	.trigger = r36s_audio_trigger,
	.sync_stop = r36s_audio_sync_stop,
	.ack = r36s_audio_ack,
	.pointer = r36s_audio_pointer,
};

/* "Master Playback Volume": the block's own volume byte, 0-255 */
static int r36s_vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = 1;
	ui->value.integer.min = 0;
	ui->value.integer.max = 255;
	return 0;
}

static int r36s_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct r36s_audio *a = snd_kcontrol_chip(kc);

	uv->value.integer.value[0] = a->volume;
	return 0;
}

static int r36s_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uv)
{
	struct r36s_audio *a = snd_kcontrol_chip(kc);
	long v = uv->value.integer.value[0];

	if (v < 0 || v > 255)
		return -EINVAL;
	if (v == a->volume)
		return 0;
	a->volume = v;
	mutex_lock(&a->lock);
	if (a->rate && audio_clock_on(a))
		writeb(v, a->snd + SND_VOL);
	mutex_unlock(&a->lock);
	return 1;
}

static const struct snd_kcontrol_new r36s_vol_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Master Playback Volume",
	.info = r36s_vol_info,
	.get = r36s_vol_get,
	.put = r36s_vol_put,
};

static ssize_t regs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct r36s_audio *a = dev_get_drvdata(dev);
	static const u16 sys_regs[] = { SYS_CHIPID, SYS_GATE1, SYS_I2S_SRC, SYS_RESET1, SYS_PADCLK,
		SYS_EJTAG, SYS_I2SDIV, SYS_I2SCTL, SYS_APLL0, SYS_APLL_F, SYS_APLL_M };
	static const u8 snd_regs[] = { SND_CTRL, SND_IRQ_PERIOD, SND_IRQ_UNDER, SND_RING,
		SND_RINGCFG, SND_WR, SND_FMT, SND_PERIOD, SND_FADE };
	int n = 0, i;

	mutex_lock(&a->lock);
	for (i = 0; i < ARRAY_SIZE(sys_regs); i++)
		n += sysfs_emit_at(buf, n, "sys+0x%03x %08x\n", sys_regs[i], readl(a->sys + sys_regs[i]));
	n += sysfs_emit_at(buf, n, "pinmux L03 %u L04 %u L05 %u L19 %u\n", readb(a->sys + SYS_PINMUX(3)),
			   readb(a->sys + SYS_PINMUX(4)), readb(a->sys + SYS_PINMUX(5)),
			   readb(a->sys + SYS_PINMUX(19)));
	if (audio_clock_on(a)) {
		for (i = 0; i < ARRAY_SIZE(snd_regs); i++)
			n += sysfs_emit_at(buf, n, "snd+0x%02x %08x\n", snd_regs[i], readl(a->snd + snd_regs[i]));
		n += sysfs_emit_at(buf, n, "rd %u wr %u vol %u now %u\n", readw(a->snd + SND_RD),
				   readw(a->snd + SND_WR), readb(a->snd + SND_VOL), readb(a->snd + SND_VOLNOW));
	} else {
		n += sysfs_emit_at(buf, n, "SND0 not read: audio clock gated (GATE1 bit 15)\n");
	}
	n += sysfs_emit_at(buf, n, "rate %u running %d volume %u\n", a->rate, a->running, a->volume);
	mutex_unlock(&a->lock);
	return n;
}
static DEVICE_ATTR_ADMIN_RO(regs);

static ssize_t headphones_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct r36s_audio *a = dev_get_drvdata(dev);

	if (!a->hp)
		return sysfs_emit(buf, "no headphone pin described\n");
	return sysfs_emit(buf, "level=%d plugged_level=%d in=%d amp=%d\n",
			  gpiod_get_raw_value(a->hp), a->hp_plugged, hp_in(a), a->amp_on);
}

static ssize_t headphones_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t len)
{
	struct r36s_audio *a = dev_get_drvdata(dev);

	if (!a->hp)
		return -ENODEV;
	if (sysfs_streq(buf, "0"))
		a->hp_plugged = 0;
	else if (sysfs_streq(buf, "1"))
		a->hp_plugged = 1;
	else if (sysfs_streq(buf, "off"))
		a->hp_plugged = -1;
	else
		return -EINVAL;
	return len;
}
static DEVICE_ATTR_ADMIN_RW(headphones);

static struct attribute *r36s_audio_attrs[] = {
	&dev_attr_regs.attr,
	&dev_attr_headphones.attr,
	NULL
};
ATTRIBUTE_GROUPS(r36s_audio);

static int r36s_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct r36s_audio *a;
	struct snd_card *card;
	struct snd_pcm *pcm;
	int ret;

	ret = snd_devm_card_new(dev, SNDRV_DEFAULT_IDX1, "R36S", THIS_MODULE, sizeof(*a), &card);
	if (ret)
		return ret;
	a = card->private_data;
	a->dev = dev;
	a->card = card;
	a->volume = DEFAULT_VOLUME;
	mutex_init(&a->lock);
	hrtimer_setup(&a->timer, r36s_audio_poll, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	a->snd = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(a->snd))
		return PTR_ERR(a->snd);
	/* shared with every other block: mapped, never claimed */
	a->sys = devm_ioremap(dev, SYS_BASE, SYS_SIZE);
	if (!a->sys)
		return -ENOMEM;
	/* AS-IS: probing must not change L19 */
	a->mute = devm_gpiod_get(dev, "mute", GPIOD_ASIS);
	if (IS_ERR(a->mute))
		return dev_err_probe(dev, PTR_ERR(a->mute), "mute-gpios (L19)\n");
	/* L15 headphone detect: an input on the stock system too; only read */
	a->hp_plugged = -1;
	a->hp = devm_gpiod_get_optional(dev, "hp-detect", GPIOD_IN);
	if (IS_ERR(a->hp))
		return dev_err_probe(dev, PTR_ERR(a->hp), "hp-detect-gpios (L15)\n");
	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	strscpy(card->driver, "nzeus-r36s", sizeof(card->driver));
	strscpy(card->shortname, "R36S", sizeof(card->shortname));
	strscpy(card->longname, "NZEUS R36S speaker (HiChip I2S, TM8211)", sizeof(card->longname));
	ret = snd_pcm_new(card, "R36S", 0, 1, 0, &pcm);
	if (ret)
		return ret;
	pcm->private_data = a;
	strscpy(pcm->name, "R36S speaker", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &r36s_audio_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, dev, 64 * 1024, 64 * 1024);
	ret = snd_ctl_add(card, snd_ctl_new1(&r36s_vol_control, a));
	if (ret)
		return ret;
	ret = snd_card_register(card);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, a);
	dev_info(dev, "sound card R36S ready (nothing set up until the first playback)\n");
	return 0;
}

static const struct of_device_id r36s_audio_of_match[] = {
	{ .compatible = "nzeus,r36s-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, r36s_audio_of_match);

static struct platform_driver r36s_audio_driver = {
	.probe = r36s_audio_probe,
	.driver = {
		.name = "nzeus-r36s-audio",
		.of_match_table = r36s_audio_of_match,
		.dev_groups = r36s_audio_groups,
	},
};
module_platform_driver(r36s_audio_driver);

MODULE_DESCRIPTION("NZEUS R36S speaker: ALSA playback through the HiChip I2S block");
MODULE_LICENSE("GPL");
