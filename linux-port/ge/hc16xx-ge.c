// SPDX-License-Identifier: GPL-2.0
/*
 * HiChip HC16xx 2D graphics engine (GE).
 *
 * The engine reads a queue of 32-bit command words from memory and draws
 * stretched, blended and copied rectangles. This driver owns the block (clock,
 * reset, registers) and one DMA-coherent command queue that a single process
 * maps and fills. The words are not interpreted here - the players document
 * them. Pictures are read from the framebuffer, which userspace already maps.
 *
 * Register map and set-up sequence come from the factory kernel's driver and
 * its libge, checked on the console (2026-11-27): registers at 0x18806000
 * (0x2000 bytes), clock gate bit 4 (0 = clock on), reset bit 4. A queue is
 * finished when the engine's read pointer has moved past the last word and the
 * busy bit has dropped.
 *
 * At the end of a queue the engine also sets bit 0 of the status register and
 * raises intc line 4 until that value is written back, as the factory
 * interrupt handler does. Checked on the console: with the handler in place
 * 1000 queues waited 60 us on average, against the 100 ms sleep each would
 * have hit without it. The wait still ends on the registers, and polls them
 * for the full timeout when no interrupt is available or none comes.
 *
 * The block is put into a known state at probe, open, reset, timeout and
 * release, so nothing here relies on what ran before. A second open is
 * refused: the queue and its addresses are shared state.
 *
 * Opening the device is a grant of physical memory access. The queue holds
 * physical addresses that the engine reads and writes without any check, so
 * whoever fills it can overwrite any memory, kernel included. The node is
 * therefore root-only (0600), and open() also requires CAP_SYS_RAWIO, as
 * /dev/mem does.
 */
#include <linux/capability.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <uapi/linux/hc16xx-ge.h>

#define GE_CTL		0x00
#define GE_KICK		0x04
#define GE_STATUS	0x08
#define GE_Q_PTR	0x10
#define GE_Q_END	0x14
#define GE_VERSION	0xac
#define GE_INIT		0xf0	/* cleared after every reset, as the vendor driver does */

#define GE_STATUS_BUSY	BIT(31)
#define GE_STATUS_DONE	BIT(0)	/* set at the end of a queue, write 1 to clear */
#define GE_CTL_RUN	BIT(31)	/* both set, in this order, before a kick */
#define GE_CTL_EN	BIT(17)
#define GE_KICK_START	2	/* GE_KICK bits 1:0 */
#define GE_KICK_MASK	0x3

#define GE_TIMEOUT_US	100000
#define GE_POLL_US	50

struct hc16xx_ge {
	struct device *dev;
	void __iomem *regs;
	struct reset_control *rst;
	struct miscdevice misc;

	void *queue;
	dma_addr_t queue_phys;
	u32 version;

	int irq;		/* completion interrupt, or <= 0 to poll */
	bool irq_on;		/* requested, while the device is open */
	bool done;		/* set by the interrupt handler */
	wait_queue_head_t wq;

	struct mutex lock;	/* serialises open, release and the ioctls */
	bool opened;
	bool busy;
	u32 q_end;		/* physical address of the last queued word */
};

static inline struct hc16xx_ge *to_hc16xx_ge(struct file *file)
{
	return container_of(file->private_data, struct hc16xx_ge, misc);
}

/* The read pointer has passed the last word and the engine is idle. */
static bool hc16xx_ge_idle(struct hc16xx_ge *ge)
{
	return readl(ge->regs + GE_Q_PTR) == ge->q_end + 4 &&
	       !(readl(ge->regs + GE_STATUS) & GE_STATUS_BUSY);
}

/*
 * No leftovers from an earlier queue or from whatever the boot firmware left
 * in the engine.
 */
static void hc16xx_ge_setup(struct hc16xx_ge *ge)
{
	if (reset_control_reset(ge->rst))
		dev_warn(ge->dev, "reset failed\n");

	writel(0, ge->regs + GE_INIT);
	writel(readl(ge->regs + GE_STATUS), ge->regs + GE_STATUS);	/* write 1 to clear */
	writel(readl(ge->regs + GE_CTL) | GE_CTL_EN, ge->regs + GE_CTL);
	writel(readl(ge->regs + GE_CTL) | GE_CTL_RUN, ge->regs + GE_CTL);
	ge->busy = false;
	ge->q_end = 0;
}

static irqreturn_t hc16xx_ge_irq(int irq, void *data)
{
	struct hc16xx_ge *ge = data;
	u32 status = readl(ge->regs + GE_STATUS);

	if (!(status & GE_STATUS_DONE))
		return IRQ_NONE;
	writel(status, ge->regs + GE_STATUS);
	WRITE_ONCE(ge->done, true);
	wake_up(&ge->wq);

	return IRQ_HANDLED;
}

static int hc16xx_ge_wait(struct hc16xx_ge *ge)
{
	unsigned long poll_us = GE_TIMEOUT_US;
	u32 status;
	int ret;

	if (!ge->busy)
		return 0;

	/*
	 * Sleep until the interrupt, then confirm with the registers. Should
	 * the interrupt not come, the full timeout is polled instead.
	 */
	if (ge->irq_on &&
	    wait_event_timeout(ge->wq, READ_ONCE(ge->done), usecs_to_jiffies(GE_TIMEOUT_US)))
		poll_us = GE_POLL_US * 20;

	ret = readl_poll_timeout(ge->regs + GE_STATUS, status, hc16xx_ge_idle(ge),
				 GE_POLL_US, poll_us);
	if (ret) {
		dev_err(ge->dev, "queue did not finish, resetting\n");
		hc16xx_ge_setup(ge);
		return ret;
	}
	ge->busy = false;

	return 0;
}

static int hc16xx_ge_open(struct inode *inode, struct file *file)
{
	struct hc16xx_ge *ge = to_hc16xx_ge(file);
	int ret = 0;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	mutex_lock(&ge->lock);
	if (ge->opened) {
		ret = -EBUSY;
	} else {
		hc16xx_ge_setup(ge);
		ge->opened = true;
		/*
		 * Only while open: the players may still drive the engine
		 * through /dev/mem, and the intc flow handler cannot mask a line.
		 */
		if (ge->irq > 0 && !request_irq(ge->irq, hc16xx_ge_irq, 0, "hc16xx-ge", ge))
			ge->irq_on = true;
	}
	mutex_unlock(&ge->lock);

	return ret;
}

static int hc16xx_ge_release(struct inode *inode, struct file *file)
{
	struct hc16xx_ge *ge = to_hc16xx_ge(file);

	mutex_lock(&ge->lock);
	hc16xx_ge_wait(ge);
	hc16xx_ge_setup(ge);
	if (ge->irq_on)
		free_irq(ge->irq, ge);
	ge->irq_on = false;
	ge->opened = false;
	mutex_unlock(&ge->lock);

	return 0;
}

static int hc16xx_ge_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct hc16xx_ge *ge = to_hc16xx_ge(file);
	size_t size = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff || size > HC16XX_GE_QUEUE_SIZE)
		return -EINVAL;

	return dma_mmap_coherent(ge->dev, vma, ge->queue, ge->queue_phys, size);
}

static long hc16xx_ge_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct hc16xx_ge *ge = to_hc16xx_ge(file);
	struct hc16xx_ge_info info;
	struct hc16xx_ge_kick kick;
	long ret = 0;

	switch (cmd) {
	case HC16XX_GE_GET_INFO:
		info.version = ge->version;
		info.queue_phys = (u32)ge->queue_phys;
		info.queue_size = HC16XX_GE_QUEUE_SIZE;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;

	case HC16XX_GE_KICK:
		if (copy_from_user(&kick, (void __user *)arg, sizeof(kick)))
			return -EFAULT;
		if ((kick.start | kick.end) & 3 || kick.start > kick.end ||
		    kick.end > HC16XX_GE_QUEUE_SIZE - 4)
			return -EINVAL;

		mutex_lock(&ge->lock);
		if (ge->busy) {
			ret = -EBUSY;
		} else {
			ge->q_end = ge->queue_phys + kick.end;
			ge->busy = true;
			WRITE_ONCE(ge->done, false);
			writel(ge->queue_phys + kick.start, ge->regs + GE_Q_PTR);
			writel(ge->q_end, ge->regs + GE_Q_END);
			writel(readl(ge->regs + GE_CTL) | GE_CTL_RUN, ge->regs + GE_CTL);
			writel(readl(ge->regs + GE_CTL) | GE_CTL_EN, ge->regs + GE_CTL);
			writel((readl(ge->regs + GE_KICK) & ~GE_KICK_MASK) | GE_KICK_START,
			       ge->regs + GE_KICK);
		}
		mutex_unlock(&ge->lock);
		return ret;

	case HC16XX_GE_WAIT:
		mutex_lock(&ge->lock);
		ret = hc16xx_ge_wait(ge);
		mutex_unlock(&ge->lock);
		return ret;

	case HC16XX_GE_RESET:
		mutex_lock(&ge->lock);
		hc16xx_ge_setup(ge);
		mutex_unlock(&ge->lock);
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations hc16xx_ge_fops = {
	.owner		= THIS_MODULE,
	.open		= hc16xx_ge_open,
	.release	= hc16xx_ge_release,
	.mmap		= hc16xx_ge_mmap,
	.unlocked_ioctl	= hc16xx_ge_ioctl,
	.llseek		= noop_llseek,
};

static int hc16xx_ge_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hc16xx_ge *ge;
	int ret;

	ge = devm_kzalloc(dev, sizeof(*ge), GFP_KERNEL);
	if (!ge)
		return -ENOMEM;
	ge->dev = dev;
	mutex_init(&ge->lock);
	init_waitqueue_head(&ge->wq);

	ge->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ge->regs))
		return PTR_ERR(ge->regs);

	/* devm_clk_get_enabled() keeps the gate open for the driver's lifetime. */
	ret = PTR_ERR_OR_ZERO(devm_clk_get_enabled(dev, "ge"));
	if (ret)
		return dev_err_probe(dev, ret, "no clock\n");

	ge->irq = platform_get_irq_optional(pdev, 0);
	if (ge->irq < 0 && ge->irq != -ENXIO)
		return dev_err_probe(dev, ge->irq, "bad interrupt\n");

	ge->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(ge->rst))
		return dev_err_probe(dev, PTR_ERR(ge->rst), "no reset\n");

	ge->queue = dma_alloc_coherent(dev, HC16XX_GE_QUEUE_SIZE, &ge->queue_phys,
				       GFP_KERNEL);
	if (!ge->queue)
		return -ENOMEM;

	hc16xx_ge_setup(ge);
	ge->version = readl(ge->regs + GE_VERSION);

	ge->misc.minor = MISC_DYNAMIC_MINOR;
	ge->misc.name = "hc16xx-ge";
	ge->misc.fops = &hc16xx_ge_fops;
	ge->misc.mode = 0600;
	ge->misc.parent = dev;
	ret = misc_register(&ge->misc);
	if (ret) {
		dma_free_coherent(dev, HC16XX_GE_QUEUE_SIZE, ge->queue, ge->queue_phys);
		return ret;
	}

	platform_set_drvdata(pdev, ge);
	dev_info(dev, "version %08x, queue at %pad, irq %d\n", ge->version, &ge->queue_phys,
		 ge->irq);

	return 0;
}

static void hc16xx_ge_remove(struct platform_device *pdev)
{
	struct hc16xx_ge *ge = platform_get_drvdata(pdev);

	misc_deregister(&ge->misc);
	mutex_lock(&ge->lock);
	hc16xx_ge_wait(ge);
	hc16xx_ge_setup(ge);
	mutex_unlock(&ge->lock);
	dma_free_coherent(ge->dev, HC16XX_GE_QUEUE_SIZE, ge->queue, ge->queue_phys);
}

static const struct of_device_id hc16xx_ge_of_match[] = {
	{ .compatible = "hichip,hc16xx-ge" },
	{ }
};
MODULE_DEVICE_TABLE(of, hc16xx_ge_of_match);

static struct platform_driver hc16xx_ge_driver = {
	.probe	= hc16xx_ge_probe,
	.remove	= hc16xx_ge_remove,
	.driver	= {
		.name		= "hc16xx-ge",
		.of_match_table	= hc16xx_ge_of_match,
	},
};
builtin_platform_driver(hc16xx_ge_driver);

MODULE_DESCRIPTION("HiChip HC16xx 2D graphics engine");
MODULE_LICENSE("GPL");
