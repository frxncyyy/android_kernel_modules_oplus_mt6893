// SPDX-License-Identifier: GPL-2.0+
/*
 * Mediatek Watchdog Driver
 *
 * Copyright (C) 2014 Matthias Brugger
 *
 * Matthias Brugger <matthias.bgg@gmail.com>
 *
 * Based on sunxi_wdt.c
 */

#include <dt-bindings/reset/mt2712-resets.h>
#include <dt-bindings/reset/mt8183-resets.h>
#include <dt-bindings/reset/mt8192-resets.h>
#include <dt-bindings/reset/mt8195-resets.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/panic_notifier.h>
#include <linux/platform_device.h>
#include <linux/rcupdate.h>
#include <linux/reboot.h>
#include <linux/reset-controller.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/watchdog.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>

#define WDT_MAX_TIMEOUT		31
#define WDT_MIN_TIMEOUT		2
#define WDT_LENGTH_TIMEOUT(n)	((n) << 5)

#define WDT_LENGTH		0x04
#define WDT_LENGTH_KEY		0x8

#define WDT_RST			0x08
#define WDT_RST_RELOAD		0x1971

#define WDT_MODE		0x00
#define WDT_MODE_EN		(1 << 0)
#define WDT_MODE_EXT_POL_LOW	(0 << 1)
#define WDT_MODE_EXT_POL_HIGH	(1 << 1)
#define WDT_MODE_EXRST_EN	(1 << 2)
#define WDT_MODE_IRQ_EN		(1 << 3)
#define WDT_MODE_AUTO_START	(1 << 4)
#define WDT_MODE_DUAL_EN	(1 << 6)
#define WDT_MODE_KEY		0x22000000

#if IS_ENABLED(CONFIG_GRT_HYPERVISOR)
#define WDT_STATUS		0x0C
#define WDT_STATUS_SWWDT_RST	(1 << 30)

#define WDT_NONRST_REG		0x20
#endif

#define WDT_SWRST		0x14
#define WDT_SWRST_KEY		0x1209

#define WDT_SWSYSRST		0x18U
#define WDT_SWSYS_RST_KEY	0x88000000

#define DRV_NAME		"mtk-wdt"
#define DRV_VERSION		"1.0"

#if IS_ENABLED(CONFIG_GRT_HYPERVISOR)
static void __iomem *toprgu_base;
#endif
static bool nowayout = WATCHDOG_NOWAYOUT;
static unsigned int timeout;

struct mtk_wdt_dev {
	struct watchdog_device wdt_dev;
	void __iomem *wdt_base;
	spinlock_t lock; /* protects WDT_SWSYSRST reg */
	struct reset_controller_dev rcdev;
};

struct mtk_wdt_data {
	int toprgu_sw_rst_num;
};

/*
 * DEBUG ONLY -- see mtk_wdt_dbg_trap() near the bottom of this file.
 *
 * dbg_wdt    : the probed device, so the trap can reach the RGU registers
 *              from any context (reboot notifier, panic, softirq).
 * dbg_frozen : once set, this driver stops petting the hardware watchdog,
 *              so the pending timeout is guaranteed to expire.
 * dbg_flags  : which hooks have fired; mirrored into WDT_DBG_NONRST_REG.
 * dbg_secs   : seconds since probe; also mirrored, so the preloader print
 *              tells us how far the kernel got before it was reset.
 */
static struct mtk_wdt_dev *dbg_wdt;
static bool dbg_frozen;
static u32 dbg_flags;
static unsigned int dbg_secs;

/* how long to wait for the hardware watchdog once the trap has fired */
#define DBG_TRAP_TIMEOUT_S	4
/* unconditional capture point, in case the boot survives longer than before */
#define DBG_HANG_AFTER_S	90
/* when to start claiming "abnormal reset" -- before the known ~29-35 s death */
#define DBG_ARCHIVE_HINT_S	20

/*
 * TOPRGU scratch register that survives a reset; the preloader prints it as
 * "[RGU] NONRST_REG:" on the following boot, which is a channel that works
 * even for the clean software resets that destroy the kernel console.
 * It reads 0x0 after every reset we have captured so far, so the low 24 bits
 * are ours to use.  The top bits are not: on the one boot that ended in a
 * hardware watchdog timeout (boot-tee-nqfix) the preloader reported
 * NONRST_REG 0xA0000000 next to STA 0xE0000000, i.e. firmware mirrors the
 * watchdog status here.  DBG_NRST_ABNORMAL replays that value on the theory
 * that it is what makes LK archive the console into expdb.
 */
#define WDT_DBG_NONRST_REG	0x20
#define DBG_NRST_MAGIC		0x00DB0000
#define DBG_NRST_ABNORMAL	0xA0000000

#define DBG_F_PROBE		0x01
#define DBG_F_REBOOT_NB		0x02
#define DBG_F_RESTART_NB	0x04
#define DBG_F_PANIC_NB		0x08
#define DBG_F_TIMER		0x10
#define DBG_F_ARMED		0x20
#define DBG_F_HINT		0x40

/*
 * DEBUG ONLY -- write the kernel log to the expdb partition ourselves.
 *
 * Every indirect channel has now failed.  The DRAM ramoops region is re-init'd
 * by LK on a clean reset; the expdb console archive at 0x091000 is only written
 * for abnormal resets; and NONRST_REG does not survive at all -- the preloader
 * log shows it setting NONRST_REG to 0x40000000 and the next boot reading back
 * 0x0, so the 0xA0000000 seen after the one hardware watchdog timeout must be
 * written by firmware while handling that reset, not carried over from Linux.
 *
 * So stop relying on anyone else's archive and write the log to flash from
 * here, the way MTK's own log_store driver does: resolve the expdb partition,
 * pull the kernel ring buffer with kmsg_dump_get_buffer() and push it out with
 * a single bio.  This needs no firmware cooperation and, because it runs
 * periodically, it does not need to know which code issues the reset -- the
 * snapshot on flash is at most DBG_DUMP_PERIOD_S seconds older than the death.
 *
 * The two slots alternate so a torn write cannot destroy the previous good
 * snapshot.  Both sit in the unused middle of the 40 MiB partition, clear of
 * the 0x091000 archive area and the pl_lk log at 0x2600000.
 */
#define DBG_EXPDB_PATH		"/dev/block/by-name/expdb"
#define DBG_DUMP_MAGIC		"MTKWDT-DBGDUMP"
#define DBG_DUMP_BYTES		(768 * 1024)
#define DBG_DUMP_HDR		512
#define DBG_DUMP_SLOT0		0x01000000ULL
#define DBG_DUMP_SLOT1		0x01100000ULL

/*
 * The reset is issued from outside Linux, so also archive the EL3 side.  ATF
 * keeps its console in a ring buffer inside the "mediatek,atf-log-reserved"
 * carveout; that is what MTK's own atf_logger driver exports as /proc/atf_log.
 * We only ever read it -- never touch atf_read_offset -- so loading atf_logger
 * alongside stays harmless.  Layout of the control block at offset 0, from
 * drivers/misc/mediatek/atf/atf_log.c:
 *
 *	u64 atf_log_addr;		0x00
 *	u64 atf_log_size;		0x08
 *	u32 atf_write_offset;		0x10
 *	u32 atf_read_offset;		0x14
 *	u64 atf_crash_log_addr;		0x18
 *	u64 atf_crash_log_size;		0x20
 *	u32 atf_total_write_count;	0x28
 *	u32 atf_crash_flag;		0x2c
 *
 * and the ring itself starts ATF_LOG_CTRL_BUF_SIZE (512) bytes in.
 */
#define DBG_ATF_KEY		"mediatek,atf-log-reserved"
#define DBG_ATF_CTRL_SIZE	512
#define DBG_ATF_MAX		(192 * 1024)

static void __iomem *dbg_atf_base;
static u32 dbg_atf_ring_len;
#define DBG_DUMP_START_S	10
#define DBG_DUMP_PERIOD_S	1

/*
 * Raw RGU registers sampled once a second into the log.  The reset we are
 * chasing sets STA bit30, which the preloader labels "rst from: kernel", yet no
 * Linux reboot path runs -- so watch the request registers for a subsystem
 * (SPM, thermal, SCP, ADSP, modem, GPU-EB) raising a reset request behind our
 * back.  Read-only, and deliberately skipping the two trigger registers
 * WDT_RST (0x08) and WDT_SWRST (0x14).
 */
static const u8 dbg_rgu_off[] = {
	0x00, 0x04, 0x0c, 0x10, 0x18, 0x1c, 0x20, 0x24,
	0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x44,
};
static u32 dbg_rgu_prev[ARRAY_SIZE(dbg_rgu_off)];

static char *dbg_dump_buf;
static unsigned int dbg_dump_seq;
static struct delayed_work dbg_dump_work;

static const struct mtk_wdt_data mt2712_data = {
	.toprgu_sw_rst_num = MT2712_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8183_data = {
	.toprgu_sw_rst_num = MT8183_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8192_data = {
	.toprgu_sw_rst_num = MT8192_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8195_data = {
	.toprgu_sw_rst_num = MT8195_TOPRGU_SW_RST_NUM,
};

static int toprgu_reset_update(struct reset_controller_dev *rcdev,
			       unsigned long id, bool assert)
{
	unsigned int tmp;
	unsigned long flags;
	struct mtk_wdt_dev *data =
		 container_of(rcdev, struct mtk_wdt_dev, rcdev);

	spin_lock_irqsave(&data->lock, flags);

	tmp = readl(data->wdt_base + WDT_SWSYSRST);
	if (assert)
		tmp |= BIT(id);
	else
		tmp &= ~BIT(id);
	tmp |= WDT_SWSYS_RST_KEY;
	writel(tmp, data->wdt_base + WDT_SWSYSRST);

	spin_unlock_irqrestore(&data->lock, flags);

	return 0;
}

static int toprgu_reset_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	return toprgu_reset_update(rcdev, id, true);
}

static int toprgu_reset_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	return toprgu_reset_update(rcdev, id, false);
}

static int toprgu_reset(struct reset_controller_dev *rcdev,
			unsigned long id)
{
	int ret;

	ret = toprgu_reset_assert(rcdev, id);
	if (ret)
		return ret;

	return toprgu_reset_deassert(rcdev, id);
}

static const struct reset_control_ops toprgu_reset_ops = {
	.assert = toprgu_reset_assert,
	.deassert = toprgu_reset_deassert,
	.reset = toprgu_reset,
};

static int toprgu_register_reset_controller(struct platform_device *pdev,
					    int rst_num)
{
	int ret;
	struct mtk_wdt_dev *mtk_wdt = platform_get_drvdata(pdev);

	spin_lock_init(&mtk_wdt->lock);

	mtk_wdt->rcdev.owner = THIS_MODULE;
	mtk_wdt->rcdev.nr_resets = rst_num;
	mtk_wdt->rcdev.ops = &toprgu_reset_ops;
	mtk_wdt->rcdev.of_node = pdev->dev.of_node;
	ret = devm_reset_controller_register(&pdev->dev, &mtk_wdt->rcdev);
	if (ret != 0)
		dev_err(&pdev->dev,
			"couldn't register wdt reset controller: %d\n", ret);
	return ret;
}

static int mtk_wdt_restart(struct watchdog_device *wdt_dev,
			   unsigned long action, void *data)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base;

	wdt_base = mtk_wdt->wdt_base;

	while (1) {
		writel(WDT_SWRST_KEY, wdt_base + WDT_SWRST);
		mdelay(5);
	}

	return 0;
}

static int mtk_wdt_ping(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;

	/* DEBUG ONLY -- let the pending timeout run out. */
	if (unlikely(dbg_frozen))
		return 0;

	iowrite32(WDT_RST_RELOAD, wdt_base + WDT_RST);
	pr_info("[wdtk] kick watchdog\n");

	return 0;
}

static int mtk_wdt_set_timeout(struct watchdog_device *wdt_dev,
				unsigned int timeout)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg;

	wdt_dev->timeout = timeout;
	/*
	 * In dual mode, irq will be triggered at timeout / 2
	 * the real timeout occurs at timeout
	 */
	if (wdt_dev->pretimeout)
		wdt_dev->pretimeout = timeout / 2;

	/*
	 * One bit is the value of 512 ticks
	 * The clock has 32 KHz
	 */
	reg = WDT_LENGTH_TIMEOUT((timeout - wdt_dev->pretimeout) << 6)
			| WDT_LENGTH_KEY;
	iowrite32(reg, wdt_base + WDT_LENGTH);

	mtk_wdt_ping(wdt_dev);

	return 0;
}

static void mtk_wdt_init(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base;

	wdt_base = mtk_wdt->wdt_base;

	if (readl(wdt_base + WDT_MODE) & WDT_MODE_EN) {
		set_bit(WDOG_HW_RUNNING, &wdt_dev->status);
		mtk_wdt_set_timeout(wdt_dev, wdt_dev->timeout);
	}
}

static int mtk_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg;

	/* DEBUG ONLY -- refuse to disarm once the trap has fired. */
	if (unlikely(dbg_frozen))
		return 0;

	reg = readl(wdt_base + WDT_MODE);
	reg &= ~WDT_MODE_EN;
	reg |= WDT_MODE_KEY;
	iowrite32(reg, wdt_base + WDT_MODE);

	clear_bit(WDOG_HW_RUNNING, &wdt_dev->status);

	return 0;
}

static int mtk_wdt_start(struct watchdog_device *wdt_dev)
{
	u32 reg;
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	int ret;

	ret = mtk_wdt_set_timeout(wdt_dev, wdt_dev->timeout);
	if (ret < 0)
		return ret;

	reg = ioread32(wdt_base + WDT_MODE);
	if (wdt_dev->pretimeout)
		reg |= (WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	else
		reg &= ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	reg |= (WDT_MODE_EN | WDT_MODE_KEY);
	iowrite32(reg, wdt_base + WDT_MODE);

	set_bit(WDOG_HW_RUNNING, &wdt_dev->status);

	return 0;
}

static int mtk_wdt_set_pretimeout(struct watchdog_device *wdd,
				  unsigned int timeout)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdd);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg = ioread32(wdt_base + WDT_MODE);

	if (timeout && !wdd->pretimeout) {
		wdd->pretimeout = wdd->timeout / 2;
		reg |= (WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	} else if (!timeout && wdd->pretimeout) {
		wdd->pretimeout = 0;
		reg &= ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	} else {
		return 0;
	}

	reg |= WDT_MODE_KEY;
	iowrite32(reg, wdt_base + WDT_MODE);

	return mtk_wdt_set_timeout(wdd, wdd->timeout);
}

static irqreturn_t mtk_wdt_isr(int irq, void *arg)
{
	struct watchdog_device *wdd = arg;

	watchdog_notify_pretimeout(wdd);

	return IRQ_HANDLED;
}

static const struct watchdog_info mtk_wdt_info = {
	.identity	= DRV_NAME,
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_info mtk_wdt_pt_info = {
	.identity	= DRV_NAME,
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_PRETIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops mtk_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= mtk_wdt_start,
	.stop		= mtk_wdt_stop,
	.ping		= mtk_wdt_ping,
	.set_timeout	= mtk_wdt_set_timeout,
	.set_pretimeout	= mtk_wdt_set_pretimeout,
	.restart	= mtk_wdt_restart,
};

#if IS_ENABLED(CONFIG_GRT_HYPERVISOR)
void mtk_wdt_set_sw_rst_status(void)
{
	u32 reg;

	if (!toprgu_base) {
		pr_info("%s: get toprgu base failed\n", __func__);
		return;
	}

	reg = ioread32(toprgu_base + WDT_STATUS);
	reg |= WDT_STATUS_SWWDT_RST;
	iowrite32(reg, toprgu_base + WDT_NONRST_REG);
}
EXPORT_SYMBOL(mtk_wdt_set_sw_rst_status);
#endif

/*
 * DEBUG ONLY -- investigation of the ~35 s reset on the MT6893 6.6 port.
 *
 * The problem is purely one of visibility.  Every 6.6 boot so far has ended in
 * a *clean* software reset (RGU "rst from: kernel", STA bit30, BOOT_REASON 4).
 * LK treats that as a normal reboot: it neither snapshots the console into the
 * expdb partition nor preserves the DRAM ramoops region, so the kernel log for
 * the interesting last second is destroyed by the very reset we want to
 * explain.  The one boot that *did* leave a 388 KiB console behind in expdb
 * (boot-tee-nqfix) got there by timing out the hardware watchdog -- an
 * abnormal reset, STA bit31, which LK does archive.
 *
 * Forcing a panic instead does not work: with panic=0 panic() hangs, but the
 * previous experiment showed the notifier is never even reached, so whatever
 * resets us either bypasses the reboot notifier chain or does not come from
 * Linux at all.
 *
 * So stop trying to guess the path and cover all of them.  Every hook below
 * funnels into mtk_wdt_dbg_trap(), which
 *
 *   1. stops petting the hardware watchdog (dbg_frozen, honoured by
 *      mtk_wdt_ping() and mtk_wdt_stop()),
 *   2. reprograms the RGU for a short single-stage reset-on-timeout, and
 *   3. names the task that got us here.
 *
 * The caller then spins, so nothing else can issue a clean reset first.  The
 * watchdog fires a few seconds later, the reset is abnormal, and LK archives
 * the console -- including the trap message and the backtrace.
 *
 * Hooks, in the order they can plausibly fire:
 *
 *   reboot notifier, INT_MAX   orderly kernel_restart()/kernel_power_off()
 *   restart handler, prio 255  emergency_restart(), which skips the notifiers,
 *                              and preempts PSCI (129) and mtk_wdt (128)
 *   panic notifier             any panic, so it no longer waits ~31 s
 *   timer, DBG_HANG_AFTER_S    unconditional capture if the boot lives longer
 *
 * If the device still comes back with STA bit30 and an empty expdb after all
 * of this, then the reset is not being issued by Linux -- look at ATF/TEE.
 */
static void mtk_wdt_dbg_map_atf(struct device *dev)
{
	struct device_node *np;
	struct reserved_mem *rmem;
	u64 ring;

	np = of_find_compatible_node(NULL, NULL, DBG_ATF_KEY);
	if (!np) {
		dev_info(dev, "mtk_wdt: no %s node\n", DBG_ATF_KEY);
		return;
	}
	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem) {
		dev_info(dev, "mtk_wdt: %s has no reserved_mem\n", DBG_ATF_KEY);
		return;
	}

	dbg_atf_base = ioremap_wc(rmem->base, rmem->size);
	if (!dbg_atf_base) {
		dev_info(dev, "mtk_wdt: cannot map ATF log carveout\n");
		return;
	}

	ring = readq_relaxed(dbg_atf_base + 0x08);
	if (!ring || ring + DBG_ATF_CTRL_SIZE > rmem->size) {
		dev_info(dev, "mtk_wdt: ATF ring size %llu implausible for %pa\n",
			 ring, &rmem->size);
		iounmap(dbg_atf_base);
		dbg_atf_base = NULL;
		return;
	}

	dbg_atf_ring_len = ring;
	dev_info(dev,
		 "mtk_wdt: ATF log carveout %pa+%pa, ring %u, write %u, total %u\n",
		 &rmem->base, &rmem->size, dbg_atf_ring_len,
		 readl_relaxed(dbg_atf_base + 0x10),
		 readl_relaxed(dbg_atf_base + 0x28));
}

/* newest DBG_ATF_MAX bytes of the EL3 ring, in chronological order */
static size_t mtk_wdt_dbg_atf(char *out, size_t size)
{
	u32 wr, rd, total, crash, len = dbg_atf_ring_len;
	size_t n, take, tail;

	if (!dbg_atf_base || !len || size < 256)
		return 0;

	wr = readl_relaxed(dbg_atf_base + 0x10);
	rd = readl_relaxed(dbg_atf_base + 0x14);
	total = readl_relaxed(dbg_atf_base + 0x28);
	crash = readl_relaxed(dbg_atf_base + 0x2c);

	n = scnprintf(out, size,
		      "\n===== ATF LOG ring=%u write=%u read=%u total_write=%u crash=%#x =====\n",
		      len, wr, rd, total, crash);
	if (wr >= len)
		return n;

	take = min3((size_t)len, (size_t)DBG_ATF_MAX, size - n - 1);
	if (take <= wr) {
		memcpy_fromio(out + n,
			      dbg_atf_base + DBG_ATF_CTRL_SIZE + wr - take, take);
		n += take;
	} else {
		tail = take - wr;
		memcpy_fromio(out + n,
			      dbg_atf_base + DBG_ATF_CTRL_SIZE + len - tail, tail);
		n += tail;
		memcpy_fromio(out + n, dbg_atf_base + DBG_ATF_CTRL_SIZE, wr);
		n += wr;
	}

	out[n] = '\0';
	return n;
}

static void mtk_wdt_dbg_who(char *out, size_t size)
{
	char parent_comm[TASK_COMM_LEN] = "?";
	pid_t parent_pid = 0;

	rcu_read_lock();
	if (current->real_parent) {
		strscpy(parent_comm, current->real_parent->comm,
			sizeof(parent_comm));
		parent_pid = task_pid_nr(current->real_parent);
	}
	rcu_read_unlock();

	scnprintf(out, size, "%s[%d] parent %s[%d]",
		  current->comm, task_pid_nr(current),
		  parent_comm, parent_pid);
}

/* must run in process context: this sleeps on block I/O */
static void mtk_wdt_dbg_dump(const char *why)
{
	static char holder;
	struct kmsg_dump_iter iter;
	struct block_device *bdev;
	char who[2 * TASK_COMM_LEN + 32];
	size_t hdr, len = 0, total;
	unsigned int pages, i;
	struct bio *bio;
	loff_t off;
	int ret;

	if (!dbg_dump_buf)
		return;

	mtk_wdt_dbg_who(who, sizeof(who));
	hdr = scnprintf(dbg_dump_buf, DBG_DUMP_HDR,
			"%s seq=%u secs=%u flags=%#x why=%s by %s\n",
			DBG_DUMP_MAGIC, dbg_dump_seq, dbg_secs, dbg_flags,
			why, who);
	memset(dbg_dump_buf + hdr, 0, DBG_DUMP_HDR - hdr);

	kmsg_dump_rewind(&iter);
	kmsg_dump_get_buffer(&iter, true, dbg_dump_buf + DBG_DUMP_HDR,
			     DBG_DUMP_BYTES - DBG_DUMP_HDR, &len);
	len += mtk_wdt_dbg_atf(dbg_dump_buf + DBG_DUMP_HDR + len,
			       DBG_DUMP_BYTES - DBG_DUMP_HDR - len);

	total = ALIGN(DBG_DUMP_HDR + len, PAGE_SIZE);
	if (total > DBG_DUMP_BYTES)
		total = DBG_DUMP_BYTES;
	memset(dbg_dump_buf + DBG_DUMP_HDR + len, 0,
	       total - DBG_DUMP_HDR - len);

	bdev = blkdev_get_by_path(DBG_EXPDB_PATH, BLK_OPEN_WRITE, &holder, NULL);
	if (IS_ERR(bdev)) {
		pr_err("mtk_wdt: DBGDUMP cannot open %s: %ld\n",
		       DBG_EXPDB_PATH, PTR_ERR(bdev));
		return;
	}

	pages = total >> PAGE_SHIFT;
	off = (dbg_dump_seq & 1) ? DBG_DUMP_SLOT1 : DBG_DUMP_SLOT0;

	bio = bio_alloc(bdev, pages, REQ_OP_WRITE | REQ_SYNC | REQ_FUA,
			GFP_KERNEL);
	if (!bio) {
		blkdev_put(bdev, &holder);
		return;
	}
	bio->bi_iter.bi_sector = off >> SECTOR_SHIFT;
	for (i = 0; i < pages; i++) {
		struct page *page =
			vmalloc_to_page(dbg_dump_buf + (i << PAGE_SHIFT));

		if (bio_add_page(bio, page, PAGE_SIZE, 0) != PAGE_SIZE) {
			pr_err("mtk_wdt: DBGDUMP bio_add_page failed at %u\n", i);
			bio_put(bio);
			blkdev_put(bdev, &holder);
			return;
		}
	}

	ret = submit_bio_wait(bio);
	bio_put(bio);
	blkdev_put(bdev, &holder);

	pr_info("mtk_wdt: DBGDUMP seq=%u why=%s log=%zu bytes -> %#llx ret=%d\n",
		dbg_dump_seq, why, len, off, ret);
	dbg_dump_seq++;
}

static void mtk_wdt_dbg_dump_fn(struct work_struct *w)
{
	mtk_wdt_dbg_dump("periodic");

	if (!dbg_frozen)
		schedule_delayed_work(&dbg_dump_work, DBG_DUMP_PERIOD_S * HZ);
}

static void mtk_wdt_dbg_sample_rgu(void)
{
	char all[ARRAY_SIZE(dbg_rgu_off) * 13 + 1];
	char chg[ARRAY_SIZE(dbg_rgu_off) * 24 + 1];
	int i, n = 0, c = 0;
	u32 v;

	if (!dbg_wdt)
		return;

	for (i = 0; i < ARRAY_SIZE(dbg_rgu_off); i++) {
		v = ioread32(dbg_wdt->wdt_base + dbg_rgu_off[i]);
		n += scnprintf(all + n, sizeof(all) - n, " %02x:%08x",
			       dbg_rgu_off[i], v);

		/* 0x20 is our own heartbeat, it changes every second by design */
		if (v != dbg_rgu_prev[i] &&
		    dbg_rgu_off[i] != WDT_DBG_NONRST_REG)
			c += scnprintf(chg + c, sizeof(chg) - c,
				       " %02x:%08x->%08x",
				       dbg_rgu_off[i], dbg_rgu_prev[i], v);
		dbg_rgu_prev[i] = v;
	}

	pr_info("mtk_wdt: RGU t=%u%s\n", dbg_secs, all);
	if (c)
		pr_warn("mtk_wdt: RGU t=%u CHANGED%s\n", dbg_secs, chg);
}

static void mtk_wdt_dbg_mark(u32 flag)
{
	u32 val;

	if (!dbg_wdt)
		return;

	dbg_flags |= flag;
	val = DBG_NRST_MAGIC | (min(dbg_secs, 255u) << 8) | (dbg_flags & 0xff);
	if (dbg_flags & DBG_F_HINT)
		val |= DBG_NRST_ABNORMAL;

	iowrite32(val, dbg_wdt->wdt_base + WDT_DBG_NONRST_REG);
}

static void mtk_wdt_dbg_trap(u32 flag, const char *why, const char *cmd)
{
	char who[2 * TASK_COMM_LEN + 32];
	void __iomem *wdt_base;
	u32 reg;

	mtk_wdt_dbg_mark(flag);

	if (!dbg_wdt || dbg_frozen)
		return;

	dbg_frozen = true;
	wdt_base = dbg_wdt->wdt_base;

	mtk_wdt_dbg_who(who, sizeof(who));
	pr_emerg("mtk_wdt: DBGTRAP via %s, cmd=%s, by %s\n",
		 why, cmd ? cmd : "<none>", who);
	dump_stack();

	/*
	 * Single stage, reset on timeout: clear IRQ_EN/DUAL_EN so the expiry
	 * resets the chip instead of raising the pretimeout interrupt (there
	 * is no pretimeout governor configured, so that would be a no-op).
	 */
	iowrite32(WDT_LENGTH_TIMEOUT(DBG_TRAP_TIMEOUT_S << 6) | WDT_LENGTH_KEY,
		  wdt_base + WDT_LENGTH);
	iowrite32(WDT_RST_RELOAD, wdt_base + WDT_RST);
	reg = ioread32(wdt_base + WDT_MODE);
	reg &= ~(WDT_MODE_IRQ_EN | WDT_MODE_DUAL_EN);
	reg |= WDT_MODE_EN | WDT_MODE_KEY;
	iowrite32(reg, wdt_base + WDT_MODE);

	pr_emerg("mtk_wdt: DBGTRAP armed, HW watchdog reset in ~%d s\n",
		 DBG_TRAP_TIMEOUT_S);
	mtk_wdt_dbg_mark(DBG_F_ARMED | DBG_F_HINT);
}

static void mtk_wdt_dbg_spin(void)
{
	pr_emerg("mtk_wdt: DBGTRAP spinning, waiting to be reset\n");
	while (1)
		cpu_relax();
}

static int mtk_wdt_dbg_reboot_call(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	const char *why;

	switch (action) {
	case SYS_RESTART:	/* == SYS_DOWN */
		why = "reboot-notifier/restart";
		break;
	case SYS_HALT:
		why = "reboot-notifier/halt";
		break;
	case SYS_POWER_OFF:
		why = "reboot-notifier/poweroff";
		break;
	default:
		why = "reboot-notifier/other";
		break;
	}

	/*
	 * Still in process context and nothing has been shut down yet, so grab
	 * a final snapshot before the trap freezes everything.  It will not
	 * contain the DBGTRAP line, but why= in the header says how we got here.
	 */
	mtk_wdt_dbg_mark(DBG_F_REBOOT_NB);
	mtk_wdt_dbg_dump(why);

	mtk_wdt_dbg_trap(DBG_F_REBOOT_NB, why, data);
	mtk_wdt_dbg_spin();

	return NOTIFY_DONE;
}

/* do_kernel_restart() passes the reboot command string as the notifier data. */
static int mtk_wdt_dbg_restart_call(struct notifier_block *nb,
				    unsigned long action, void *data)
{
	mtk_wdt_dbg_trap(DBG_F_RESTART_NB, "restart-handler", data);
	mtk_wdt_dbg_spin();

	return NOTIFY_DONE;
}

/* panic() will hang or restart on its own; just make sure the RGU is armed. */
static int mtk_wdt_dbg_panic_call(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	mtk_wdt_dbg_trap(DBG_F_PANIC_NB, "panic", data);

	return NOTIFY_DONE;
}

/* set up by mtk_wdt_dbg_arm(); armed only on this debug build */
static struct timer_list mtk_wdt_dbg_timer;

/*
 * 1 Hz heartbeat.  Every tick mirrors the elapsed seconds and the hook flags
 * into the scratch register, so the next boot's preloader print says how long
 * this kernel lived even when the console is destroyed by the reset.  At
 * DBG_ARCHIVE_HINT_S it also starts claiming an abnormal reset, in the hope
 * that this is what makes LK archive the console; at DBG_HANG_AFTER_S it gives
 * up waiting for someone else to reset us and trips the trap itself.
 */
static void mtk_wdt_dbg_timeout(struct timer_list *t)
{
	dbg_secs++;

	if (dbg_secs >= DBG_HANG_AFTER_S) {
		mtk_wdt_dbg_trap(DBG_F_TIMER, "suicide-timer", NULL);
		return;
	}

	mtk_wdt_dbg_mark(dbg_secs >= DBG_ARCHIVE_HINT_S ? DBG_F_HINT : 0);
	mtk_wdt_dbg_sample_rgu();
	mod_timer(&mtk_wdt_dbg_timer, jiffies + HZ);
}

static struct notifier_block mtk_wdt_dbg_reboot_nb = {
	.notifier_call	= mtk_wdt_dbg_reboot_call,
	.priority	= INT_MAX,
};

static struct notifier_block mtk_wdt_dbg_restart_nb = {
	.notifier_call	= mtk_wdt_dbg_restart_call,
	.priority	= 255,
};

static struct notifier_block mtk_wdt_dbg_panic_nb = {
	.notifier_call	= mtk_wdt_dbg_panic_call,
	.priority	= INT_MAX,
};

static void mtk_wdt_dbg_arm(struct device *dev, struct mtk_wdt_dev *mtk_wdt)
{
	dbg_wdt = mtk_wdt;
	mtk_wdt_dbg_mark(DBG_F_PROBE);
	mtk_wdt_dbg_map_atf(dev);

	register_reboot_notifier(&mtk_wdt_dbg_reboot_nb);
	register_restart_handler(&mtk_wdt_dbg_restart_nb);
	atomic_notifier_chain_register(&panic_notifier_list,
				      &mtk_wdt_dbg_panic_nb);
	timer_setup(&mtk_wdt_dbg_timer, mtk_wdt_dbg_timeout, 0);
	mod_timer(&mtk_wdt_dbg_timer, jiffies + HZ);

	dbg_dump_buf = vmalloc(DBG_DUMP_BYTES);
	if (dbg_dump_buf) {
		INIT_DELAYED_WORK(&dbg_dump_work, mtk_wdt_dbg_dump_fn);
		schedule_delayed_work(&dbg_dump_work, DBG_DUMP_START_S * HZ);
	} else {
		dev_err(dev, "mtk_wdt: DBGDUMP buffer allocation failed\n");
	}

	dev_info(dev,
		 "mtk_wdt: DBGTRAP v6 armed (NONRST_REG %#x, dump to " DBG_EXPDB_PATH
		 " %#llx/%#llx from %d s every %d s, hint at %d s, suicide at %d s)\n",
		 ioread32(mtk_wdt->wdt_base + WDT_DBG_NONRST_REG),
		 DBG_DUMP_SLOT0, DBG_DUMP_SLOT1,
		 DBG_DUMP_START_S, DBG_DUMP_PERIOD_S,
		 DBG_ARCHIVE_HINT_S, DBG_HANG_AFTER_S);
	mtk_wdt_dbg_sample_rgu();
}

static int mtk_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_wdt_dev *mtk_wdt;
	const struct mtk_wdt_data *wdt_data;
	int err, irq;

	mtk_wdt = devm_kzalloc(dev, sizeof(*mtk_wdt), GFP_KERNEL);
	if (!mtk_wdt)
		return -ENOMEM;

	platform_set_drvdata(pdev, mtk_wdt);

	mtk_wdt->wdt_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mtk_wdt->wdt_base))
		return PTR_ERR(mtk_wdt->wdt_base);

#if IS_ENABLED(CONFIG_GRT_HYPERVISOR)
	toprgu_base = mtk_wdt->wdt_base;
#endif

	irq = platform_get_irq(pdev, 0);
	if (irq > 0) {
		err = devm_request_irq(&pdev->dev, irq, mtk_wdt_isr, 0, "wdt_bark",
				       &mtk_wdt->wdt_dev);
		if (err)
			return err;

		mtk_wdt->wdt_dev.info = &mtk_wdt_pt_info;
		mtk_wdt->wdt_dev.pretimeout = WDT_MAX_TIMEOUT / 2;
	} else {
		if (irq == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		mtk_wdt->wdt_dev.info = &mtk_wdt_info;
	}

	mtk_wdt->wdt_dev.ops = &mtk_wdt_ops;
	mtk_wdt->wdt_dev.timeout = WDT_MAX_TIMEOUT;
	mtk_wdt->wdt_dev.max_hw_heartbeat_ms = WDT_MAX_TIMEOUT * 1000;
	mtk_wdt->wdt_dev.min_timeout = WDT_MIN_TIMEOUT;
	mtk_wdt->wdt_dev.parent = dev;

	watchdog_init_timeout(&mtk_wdt->wdt_dev, timeout, dev);
	watchdog_set_nowayout(&mtk_wdt->wdt_dev, nowayout);
	watchdog_set_restart_priority(&mtk_wdt->wdt_dev, 128);

	watchdog_set_drvdata(&mtk_wdt->wdt_dev, mtk_wdt);

	mtk_wdt_init(&mtk_wdt->wdt_dev);

#if defined(CONFIG_MEDIATEK_WATCHDOG_STOP_ON_REBOOT)
	watchdog_stop_on_reboot(&mtk_wdt->wdt_dev);
#endif
	err = devm_watchdog_register_device(dev, &mtk_wdt->wdt_dev);
	if (unlikely(err))
		return err;

	dev_info(dev, "Watchdog enabled (timeout=%d sec, nowayout=%d)\n",
		 mtk_wdt->wdt_dev.timeout, nowayout);

	/* DEBUG ONLY -- see mtk_wdt_dbg_trap() above. */
	mtk_wdt_dbg_arm(dev, mtk_wdt);

	wdt_data = of_device_get_match_data(dev);
	if (wdt_data) {
		err = toprgu_register_reset_controller(pdev,
						       wdt_data->toprgu_sw_rst_num);
		if (err)
			return err;
	}
	return 0;
}

#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_MEDIATEK_WATCHDOG_PM)
static int mtk_wdt_suspend(struct device *dev)
{
	struct mtk_wdt_dev *mtk_wdt = dev_get_drvdata(dev);

	if (watchdog_active(&mtk_wdt->wdt_dev))
		mtk_wdt_stop(&mtk_wdt->wdt_dev);

	return 0;
}

static int mtk_wdt_resume(struct device *dev)
{
	struct mtk_wdt_dev *mtk_wdt = dev_get_drvdata(dev);

	if (watchdog_active(&mtk_wdt->wdt_dev)) {
		mtk_wdt_start(&mtk_wdt->wdt_dev);
		mtk_wdt_ping(&mtk_wdt->wdt_dev);
	}

	return 0;
}

static const struct dev_pm_ops mtk_wdt_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_wdt_suspend,
				mtk_wdt_resume)
};
#endif

static const struct of_device_id mtk_wdt_dt_ids[] = {
	{ .compatible = "mediatek,mt2712-wdt", .data = &mt2712_data },
	{ .compatible = "mediatek,mt6589-wdt" },
	{ .compatible = "mediatek,mt8183-wdt", .data = &mt8183_data },
	{ .compatible = "mediatek,mt8192-wdt", .data = &mt8192_data },
	{ .compatible = "mediatek,mt8195-wdt", .data = &mt8195_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_wdt_dt_ids);

static struct platform_driver mtk_wdt_driver = {
	.probe		= mtk_wdt_probe,
	.driver		= {
		.name		= DRV_NAME,
#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_MEDIATEK_WATCHDOG_PM)
		.pm		= &mtk_wdt_pm_ops,
#endif
		.of_match_table	= mtk_wdt_dt_ids,
	},
};

module_platform_driver(mtk_wdt_driver);

module_param(timeout, uint, 0);
MODULE_PARM_DESC(timeout, "Watchdog heartbeat in seconds");

module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
			__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Brugger <matthias.bgg@gmail.com>");
MODULE_DESCRIPTION("Mediatek WatchDog Timer Driver");
MODULE_VERSION(DRV_VERSION);
