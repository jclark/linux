// SPDX-License-Identifier: GPL-2.0-only
/*
 * PTP hardware clock for the time interval counter (TIC) in the FPGA fabric
 * of the Terasic DE25-Nano (Altera Agilex 5).
 *
 * The TIC is a free-running 64-bit counter (400 MHz on the DE25-Nano) and a
 * number of input channels. A rising edge on an enabled channel puts the
 * counter value and the channel number into a FIFO, and a level interrupt is
 * high while the FIFO holds an event. Each channel is an external timestamp
 * (extts) channel of the clock. There is no network interface.
 *
 * The time of the clock is a timecounter over the TIC counter, so settime,
 * adjtime and adjfine are done in software; the counter itself never changes.
 *
 * Cross timestamps (PTP_SYS_OFFSET_PRECISE) come from the SMTG hub of an HPS
 * Ethernet MAC (Synopsys XGMAC) that is otherwise unused. The fabric feeds the
 * TIC counter into the hubs' fabric counter input, and one trigger (the MAC's
 * GPO0 output going high) makes a hub capture both the HPS system counter
 * (the ARM generic timer's counter) and the fabric counter. The hub's
 * registers are read over that MAC's own MDIO interface, at port 0x15. The
 * MAC is named by the "jclark,smtg-hub" phandle; it must be disabled in the
 * device tree, and this driver takes it out of reset and holds its clock.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/clocksource.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/timecounter.h>
#include <linux/timekeeping.h>

#include <clocksource/arm_arch_timer.h>

/* TIC registers. Every access is one 32-bit read or write. */
#define TIC_ID			0x00
#define TIC_INFO		0x04
#define TIC_FREQ		0x08
#define TIC_STATUS		0x0c
#define TIC_CTRL		0x10
#define TIC_CLEAR		0x14
#define TIC_ENABLE		0x18
#define TIC_INTERNAL		0x1c
#define TIC_NOW_LO		0x20	/* reading takes a snapshot */
#define TIC_NOW_HI		0x24
#define TIC_EVT			0x28	/* reading pops an event */
#define TIC_EVT_LO		0x2c
#define TIC_EVT_HI		0x30
#define TIC_MISSED(k)		(0x40 + 4 * (k))

#define TIC_ID_TIC2		0x54494332	/* "TIC2" */
#define TIC_INFO_NCH		GENMASK(7, 0)
#define TIC_INFO_FIFO_LOG2	GENMASK(15, 8)
#define TIC_INFO_REV		GENMASK(23, 16)
#define TIC_STATUS_LOCKED	BIT(0)
#define TIC_CTRL_IRQ_EN		BIT(0)
#define TIC_CLEAR_ALL		BIT(0)
#define TIC_EVT_VALID		BIT(31)
#define TIC_EVT_CHAN		GENMASK(7, 0)

#define TIC_MAX_CHANNELS	32

/*
 * An event's timestamp is the counter value 2 to 3 counts after the edge
 * reached the channel's synchroniser: the number of counter clock edges
 * before the input edge, plus 2. Take the 2 off.
 */
#define TIC_EVT_DELAY		2

/*
 * The timecounter must be read more often than the counter takes to make
 * cycles * mult overflow 64 bits. Choose mult and shift for 60 s and read it
 * every 10 s.
 */
#define TIC_TC_MAXSEC		60
#define TIC_TC_PERIOD		(10 * HZ)

/* XGMAC registers used for the SMTG hub (see dwxgmac2.h, stmmac_mdio.c). */
#define XGMAC_VERSION		0x110
#define XGMAC_VERSION_SNPSVER	GENMASK(7, 0)
#define XGMAC_CORE_2_20		0x22
#define XGMAC_MDIO_ADDR		0x200
#define XGMAC_MDIO_ADDR_PA	GENMASK(20, 16)
#define XGMAC_MDIO_ADDR_REG	GENMASK(4, 0)
#define XGMAC_MDIO_DATA		0x204
#define XGMAC_MDIO_DATA_BUSY	BIT(22)
#define XGMAC_MDIO_DATA_CR	GENMASK(21, 19)
#define XGMAC_MDIO_DATA_CMD	GENMASK(17, 16)
#define XGMAC_MDIO_CMD_READ	3
#define XGMAC_MDIO_DATA_DATA	GENMASK(15, 0)
#define XGMAC_MDIO_C22P		0x220
#define XGMAC_GPIO_STATUS	0x27c
#define XGMAC_GPIO_GPO0		BIT(16)

/* SMTG hub registers, 16 bits each, lowest register lowest bits. */
#define SMTG_PORT		0x15
#define SMTG_SYS_COUNTER	0x0c	/* HPS system counter, 0x0c-0x0f */
#define SMTG_FABRIC_COUNTER	0x10	/* fabric (TIC) counter, 0x10-0x13 */

struct tic_hub {
	void __iomem *base;
	u32 mdio_cr;
	struct mutex lock;	/* one trigger and its readout at a time */
};

struct tic {
	struct device *dev;
	void __iomem *base;
	unsigned int nch;
	unsigned int fifo_depth;
	u32 internal;

	struct ptp_clock_info info;
	struct ptp_clock *ptp;

	/*
	 * lock protects the timecounter and the counter snapshot (NOW_LO then
	 * NOW_HI), the channel enables and the missed-edge bookkeeping. The
	 * interrupt handler takes it.
	 */
	spinlock_t lock;
	struct cyclecounter cc;
	struct timecounter tc;
	u32 base_mult;
	u32 enabled;
	u8 missed[TIC_MAX_CHANNELS];

	struct tic_hub *hub;
};

static u32 tic_read(struct tic *tic, unsigned int off)
{
	return readl(tic->base + off);
}

/* The bridge posts writes: read back so that the write has landed. */
static void tic_write(struct tic *tic, unsigned int off, u32 val)
{
	writel(val, tic->base + off);
	readl(tic->base + off);
}

/* Called with tic->lock held. */
static u64 tic_now(struct tic *tic, struct ptp_system_timestamp *sts)
{
	u32 lo, hi;

	ptp_read_system_prets(sts);
	lo = tic_read(tic, TIC_NOW_LO);
	ptp_read_system_postts(sts);
	hi = tic_read(tic, TIC_NOW_HI);

	return (u64)hi << 32 | lo;
}

static u64 tic_cc_read(struct cyclecounter *cc)
{
	struct tic *tic = container_of(cc, struct tic, cc);

	return tic_now(tic, NULL);
}

static int tic_adjfine(struct ptp_clock_info *info, long scaled_ppm)
{
	struct tic *tic = container_of(info, struct tic, info);
	unsigned long flags;

	spin_lock_irqsave(&tic->lock, flags);
	timecounter_read(&tic->tc);
	tic->cc.mult = adjust_by_scaled_ppm(tic->base_mult, scaled_ppm);
	spin_unlock_irqrestore(&tic->lock, flags);

	return 0;
}

static int tic_adjtime(struct ptp_clock_info *info, s64 delta)
{
	struct tic *tic = container_of(info, struct tic, info);
	unsigned long flags;

	spin_lock_irqsave(&tic->lock, flags);
	timecounter_adjtime(&tic->tc, delta);
	spin_unlock_irqrestore(&tic->lock, flags);

	return 0;
}

static int tic_gettimex64(struct ptp_clock_info *info, struct timespec64 *ts,
			  struct ptp_system_timestamp *sts)
{
	struct tic *tic = container_of(info, struct tic, info);
	unsigned long flags;
	u64 ns;

	spin_lock_irqsave(&tic->lock, flags);
	ns = timecounter_cyc2time(&tic->tc, tic_now(tic, sts));
	spin_unlock_irqrestore(&tic->lock, flags);

	*ts = ns_to_timespec64(ns);

	return 0;
}

static int tic_settime64(struct ptp_clock_info *info,
			 const struct timespec64 *ts)
{
	struct tic *tic = container_of(info, struct tic, info);
	unsigned long flags;

	spin_lock_irqsave(&tic->lock, flags);
	timecounter_init(&tic->tc, &tic->cc, timespec64_to_ns(ts));
	spin_unlock_irqrestore(&tic->lock, flags);

	return 0;
}

static int tic_enable(struct ptp_clock_info *info,
		      struct ptp_clock_request *rq, int on)
{
	struct tic *tic = container_of(info, struct tic, info);
	unsigned int ch;
	unsigned long flags;
	u32 missed;

	if (rq->type != PTP_CLK_REQ_EXTTS)
		return -EOPNOTSUPP;

	/* The core has checked the index and the flags. */
	ch = rq->extts.index;

	spin_lock_irqsave(&tic->lock, flags);
	if (on) {
		/* Count only the edges missed from now on. */
		missed = tic_read(tic, TIC_MISSED(ch / 4));
		tic->missed[ch] = missed >> (8 * (ch % 4));
		tic->enabled |= BIT(ch);
	} else {
		tic->enabled &= ~BIT(ch);
	}
	tic_write(tic, TIC_ENABLE, tic->enabled);
	spin_unlock_irqrestore(&tic->lock, flags);

	return 0;
}

/* Called with tic->lock held. */
static void tic_check_missed(struct tic *tic)
{
	unsigned int k, i, ch;
	u32 val;
	u8 n;

	for (k = 0; k < DIV_ROUND_UP(tic->nch, 4); k++) {
		if (!(tic->enabled & (0xfu << (4 * k))))
			continue;
		val = tic_read(tic, TIC_MISSED(k));
		for (i = 0; i < 4; i++) {
			ch = 4 * k + i;
			if (!(tic->enabled & BIT(ch)))
				continue;
			/* The counts wrap at 256. */
			n = (u8)(val >> (8 * i)) - tic->missed[ch];
			if (!n)
				continue;
			tic->missed[ch] += n;
			dev_warn_ratelimited(tic->dev,
					     "channel %u: %u edges missed\n",
					     ch, n);
		}
	}
}

static irqreturn_t tic_irq(int irq, void *data)
{
	struct ptp_clock_event event = { .type = PTP_CLOCK_EXTTS };
	struct tic *tic = data;
	irqreturn_t ret = IRQ_NONE;
	unsigned int i, ch;
	u32 evt, lo, hi;
	u64 cyc;

	spin_lock(&tic->lock);

	/*
	 * The line is high while the FIFO holds an event, and goes low once
	 * EVT has been read with the FIFO empty. Bound the loop in case the
	 * bus returns nonsense.
	 */
	for (i = 0; i < 2 * tic->fifo_depth; i++) {
		evt = tic_read(tic, TIC_EVT);
		if (!(evt & TIC_EVT_VALID))
			break;
		ret = IRQ_HANDLED;
		lo = tic_read(tic, TIC_EVT_LO);
		hi = tic_read(tic, TIC_EVT_HI);

		ch = FIELD_GET(TIC_EVT_CHAN, evt);
		/* An event from a channel just disabled. */
		if (ch >= tic->nch || !(tic->enabled & BIT(ch)))
			continue;

		cyc = ((u64)hi << 32 | lo) - TIC_EVT_DELAY;
		event.index = ch;
		event.timestamp = timecounter_cyc2time(&tic->tc, cyc);
		ptp_clock_event(tic->ptp, &event);
	}

	if (ret == IRQ_HANDLED)
		tic_check_missed(tic);

	spin_unlock(&tic->lock);

	return ret;
}

static long tic_do_aux_work(struct ptp_clock_info *info)
{
	struct tic *tic = container_of(info, struct tic, info);
	unsigned long flags;

	spin_lock_irqsave(&tic->lock, flags);
	timecounter_read(&tic->tc);
	spin_unlock_irqrestore(&tic->lock, flags);

	return TIC_TC_PERIOD;
}

/* SMTG hub */

static int tic_hub_mdio_wait(struct tic_hub *hub)
{
	u32 val;

	return readl_poll_timeout(hub->base + XGMAC_MDIO_DATA, val,
				  !(val & XGMAC_MDIO_DATA_BUSY), 5, 10000);
}

static int tic_hub_mdio_read(struct tic_hub *hub, unsigned int reg)
{
	int ret;

	ret = tic_hub_mdio_wait(hub);
	if (ret)
		return ret;

	writel(FIELD_PREP(XGMAC_MDIO_ADDR_PA, SMTG_PORT) |
	       FIELD_PREP(XGMAC_MDIO_ADDR_REG, reg),
	       hub->base + XGMAC_MDIO_ADDR);
	writel(FIELD_PREP(XGMAC_MDIO_DATA_CR, hub->mdio_cr) |
	       FIELD_PREP(XGMAC_MDIO_DATA_CMD, XGMAC_MDIO_CMD_READ) |
	       XGMAC_MDIO_DATA_BUSY,
	       hub->base + XGMAC_MDIO_DATA);

	ret = tic_hub_mdio_wait(hub);
	if (ret)
		return ret;

	return FIELD_GET(XGMAC_MDIO_DATA_DATA,
			 readl(hub->base + XGMAC_MDIO_DATA));
}

/* Read a 64-bit capture: four 16-bit registers, lowest bits first. */
static int tic_hub_read64(struct tic_hub *hub, unsigned int first, u64 *val)
{
	int i, ret;

	*val = 0;
	for (i = 0; i < 4; i++) {
		ret = tic_hub_mdio_read(hub, first + i);
		if (ret < 0)
			return ret;
		*val |= (u64)ret << (16 * i);
	}

	return 0;
}

static void tic_hub_gpo0(struct tic_hub *hub, bool high)
{
	u32 val;

	val = readl(hub->base + XGMAC_GPIO_STATUS);
	if (high)
		val |= XGMAC_GPIO_GPO0;
	else
		val &= ~XGMAC_GPIO_GPO0;
	writel(val, hub->base + XGMAC_GPIO_STATUS);
	readl(hub->base + XGMAC_GPIO_STATUS);
}

static int tic_hub_get_time(ktime_t *device, struct system_counterval_t *system,
			    void *ctx)
{
	struct tic *tic = ctx;
	struct tic_hub *hub = tic->hub;
	u64 sys0, sys1, sys_cap, tic0, tic1, tic_cap;
	unsigned long flags;
	int ret;

	guard(mutex)(&hub->lock);

	tic_hub_gpo0(hub, false);

	spin_lock_irqsave(&tic->lock, flags);
	tic0 = tic_now(tic, NULL);
	spin_unlock_irqrestore(&tic->lock, flags);
	sys0 = arch_timer_read_counter();

	/* The rising edge of GPO0 triggers the capture. */
	tic_hub_gpo0(hub, true);
	udelay(1);

	sys1 = arch_timer_read_counter();
	spin_lock_irqsave(&tic->lock, flags);
	tic1 = tic_now(tic, NULL);
	spin_unlock_irqrestore(&tic->lock, flags);

	ret = tic_hub_read64(hub, SMTG_SYS_COUNTER, &sys_cap);
	if (!ret)
		ret = tic_hub_read64(hub, SMTG_FABRIC_COUNTER, &tic_cap);

	tic_hub_gpo0(hub, false);

	if (ret) {
		dev_err_ratelimited(tic->dev, "SMTG hub: MDIO read failed: %d\n",
				    ret);
		return ret;
	}

	/*
	 * Each capture must lie between readings of its own counter taken
	 * before and after the trigger; otherwise the hub did not capture on
	 * this trigger.
	 */
	if (sys_cap < sys0 || sys_cap > sys1 ||
	    tic_cap < tic0 || tic_cap > tic1) {
		dev_err_ratelimited(tic->dev,
				    "SMTG hub: capture outside its bracket: system counter %llu not in [%llu, %llu] or TIC %llu not in [%llu, %llu]\n",
				    sys_cap, sys0, sys1, tic_cap, tic0, tic1);
		return -EIO;
	}

	spin_lock_irqsave(&tic->lock, flags);
	*device = ns_to_ktime(timecounter_cyc2time(&tic->tc, tic_cap));
	spin_unlock_irqrestore(&tic->lock, flags);

	*system = (struct system_counterval_t) {
		.cycles = sys_cap,
		.cs_id = CSID_ARM_ARCH_COUNTER,
	};

	return 0;
}

static int tic_getcrosststamp(struct ptp_clock_info *info,
			      struct system_device_crosststamp *cts)
{
	struct tic *tic = container_of(info, struct tic, info);
	struct system_time_snapshot snap;

	/*
	 * The MDIO reads take a few hundred microseconds, so the timekeeper
	 * may start a new interval before the capture is converted. A
	 * snapshot from before the trigger lets the core interpolate.
	 */
	ktime_get_snapshot_id(cts->clock_id, &snap);

	return get_device_system_crosststamp(tic_hub_get_time, tic, &snap, cts);
}

static void tic_hub_clk_put(void *data)
{
	struct clk *clk = data;

	clk_disable_unprepare(clk);
	clk_put(clk);
}

static void tic_hub_reset_put(void *data)
{
	struct reset_control *rst = data;

	reset_control_assert(rst);
	reset_control_put(rst);
}

static int tic_hub_probe(struct tic *tic)
{
	struct device *dev = tic->dev;
	struct device_node *np __free(device_node) = NULL;
	struct reset_control *rst;
	struct tic_hub *hub;
	struct resource res;
	struct clk *clk;
	u32 val;
	int ret;

	np = of_parse_phandle(dev->of_node, "jclark,smtg-hub", 0);
	if (!np) {
		dev_info(dev, "no SMTG hub: no cross timestamps\n");
		return 0;
	}

	/* An enabled MAC belongs to its own driver. */
	if (of_device_is_available(np))
		return dev_err_probe(dev, -EBUSY,
				     "SMTG hub %pOF: the MAC is enabled\n", np);

	hub = devm_kzalloc(dev, sizeof(*hub), GFP_KERNEL);
	if (!hub)
		return -ENOMEM;
	ret = devm_mutex_init(dev, &hub->lock);
	if (ret)
		return ret;

	ret = of_address_to_resource(np, 0, &res);
	if (ret)
		return dev_err_probe(dev, ret, "SMTG hub %pOF: no registers\n",
				     np);
	hub->base = devm_ioremap_resource(dev, &res);
	if (IS_ERR(hub->base))
		return PTR_ERR(hub->base);

	val = 0;
	of_property_read_u32(np, "snps,clk-csr", &val);
	if (val > FIELD_MAX(XGMAC_MDIO_DATA_CR))
		return dev_err_probe(dev, -EINVAL,
				     "SMTG hub %pOF: snps,clk-csr %u\n",
				     np, val);
	hub->mdio_cr = val;

	/* The clock first, then out of reset. */
	clk = of_clk_get_by_name(np, "stmmaceth");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "SMTG hub %pOF: no clock\n", np);
	ret = clk_prepare_enable(clk);
	if (ret) {
		clk_put(clk);
		return dev_err_probe(dev, ret,
				     "SMTG hub %pOF: clock enable failed\n",
				     np);
	}
	ret = devm_add_action_or_reset(dev, tic_hub_clk_put, clk);
	if (ret)
		return ret;

	rst = of_reset_control_array_get_exclusive(np);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst),
				     "SMTG hub %pOF: no resets\n", np);
	ret = reset_control_deassert(rst);
	if (ret) {
		reset_control_put(rst);
		return dev_err_probe(dev, ret,
				     "SMTG hub %pOF: reset release failed\n",
				     np);
	}
	ret = devm_add_action_or_reset(dev, tic_hub_reset_put, rst);
	if (ret)
		return ret;
	fsleep(10000);

	/* Ports above 3 can be clause 22 ports from core 2.20 on. */
	val = readl(hub->base + XGMAC_VERSION);
	if (FIELD_GET(XGMAC_VERSION_SNPSVER, val) < XGMAC_CORE_2_20)
		return dev_err_probe(dev, -ENODEV,
				     "SMTG hub %pOF: XGMAC version %#x\n",
				     np, val);
	val = readl(hub->base + XGMAC_MDIO_C22P);
	writel(val | BIT(SMTG_PORT), hub->base + XGMAC_MDIO_C22P);
	readl(hub->base + XGMAC_MDIO_C22P);

	tic->hub = hub;
	tic->info.getcrosststamp = tic_getcrosststamp;
	dev_info(dev, "SMTG hub of %pOF: cross timestamps\n", np);

	return 0;
}

static const struct ptp_clock_info tic_info = {
	.owner			= THIS_MODULE,
	.name			= "DE25-Nano TIC",
	.max_adj		= 1000000,
	.supported_extts_flags	= PTP_RISING_EDGE | PTP_STRICT_FLAGS,
	.adjfine		= tic_adjfine,
	.adjtime		= tic_adjtime,
	.gettimex64		= tic_gettimex64,
	.settime64		= tic_settime64,
	.enable			= tic_enable,
	.do_aux_work		= tic_do_aux_work,
};

static void tic_ptp_unregister(void *data)
{
	struct tic *tic = data;

	ptp_clock_unregister(tic->ptp);
}

static void tic_irq_disable(void *data)
{
	struct tic *tic = data;
	unsigned long flags;

	spin_lock_irqsave(&tic->lock, flags);
	tic_write(tic, TIC_CTRL, 0);
	tic->enabled = 0;
	tic_write(tic, TIC_ENABLE, 0);
	spin_unlock_irqrestore(&tic->lock, flags);
}

static int tic_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int irq, rev;
	struct tic *tic;
	u32 val, freq, mult, shift;
	int ret;

	tic = devm_kzalloc(dev, sizeof(*tic), GFP_KERNEL);
	if (!tic)
		return -ENOMEM;
	tic->dev = dev;
	spin_lock_init(&tic->lock);
	tic->info = tic_info;

	tic->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(tic->base))
		return PTR_ERR(tic->base);

	val = tic_read(tic, TIC_ID);
	if (val != TIC_ID_TIC2)
		return dev_err_probe(dev, -ENODEV, "ID %#010x, not TIC2\n",
				     val);

	val = tic_read(tic, TIC_INFO);
	tic->nch = FIELD_GET(TIC_INFO_NCH, val);
	tic->fifo_depth = 1U << FIELD_GET(TIC_INFO_FIFO_LOG2, val);
	rev = FIELD_GET(TIC_INFO_REV, val);
	if (!tic->nch || tic->nch > TIC_MAX_CHANNELS ||
	    tic->fifo_depth > 4096)
		return dev_err_probe(dev, -ENODEV, "INFO %#010x\n", val);
	tic->internal = tic_read(tic, TIC_INTERNAL) & GENMASK(tic->nch - 1, 0);

	freq = tic_read(tic, TIC_FREQ);
	if (!freq)
		return dev_err_probe(dev, -ENODEV, "FREQ is 0\n");

	ret = readl_poll_timeout(tic->base + TIC_STATUS, val,
				 val & TIC_STATUS_LOCKED, 1000, 100000);
	if (ret)
		return dev_err_probe(dev, ret, "counter PLL not locked\n");

	/* No interrupt, no channels, nothing in the FIFO. */
	tic_write(tic, TIC_CTRL, 0);
	tic_write(tic, TIC_ENABLE, 0);
	tic_write(tic, TIC_CLEAR, TIC_CLEAR_ALL);

	clocks_calc_mult_shift(&mult, &shift, freq, NSEC_PER_SEC,
			       TIC_TC_MAXSEC);
	tic->base_mult = mult;
	tic->cc.read = tic_cc_read;
	tic->cc.mask = CYCLECOUNTER_MASK(64);
	tic->cc.mult = mult;
	tic->cc.shift = shift;
	timecounter_init(&tic->tc, &tic->cc, ktime_get_real_ns());

	ret = tic_hub_probe(tic);
	if (ret)
		return ret;

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	irq = ret;
	ret = devm_request_irq(dev, irq, tic_irq, 0, dev_name(dev), tic);
	if (ret)
		return dev_err_probe(dev, ret, "request IRQ %u\n", irq);

	tic->info.n_ext_ts = tic->nch;
	tic->ptp = ptp_clock_register(&tic->info, dev);
	if (IS_ERR(tic->ptp))
		return dev_err_probe(dev, PTR_ERR(tic->ptp),
				     "PTP clock register failed\n");
	if (!tic->ptp)
		return -ENODEV;
	ret = devm_add_action_or_reset(dev, tic_ptp_unregister, tic);
	if (ret)
		return ret;

	/*
	 * The handler uses tic->ptp, so enable the interrupt only now, and
	 * disable it before the clock is unregistered.
	 */
	tic_write(tic, TIC_CTRL, TIC_CTRL_IRQ_EN);
	ret = devm_add_action_or_reset(dev, tic_irq_disable, tic);
	if (ret)
		return ret;

	ptp_schedule_worker(tic->ptp, TIC_TC_PERIOD);

	dev_info(dev,
		 "ptp%d: %u channels (internal %#x), FIFO %u, %u Hz, revision %u\n",
		 ptp_clock_index(tic->ptp), tic->nch, tic->internal,
		 tic->fifo_depth, freq, rev);

	return 0;
}

static const struct of_device_id tic_of_match[] = {
	{ .compatible = "jclark,de25-nano-tic2" },
	{ }
};
MODULE_DEVICE_TABLE(of, tic_of_match);

static struct platform_driver tic_driver = {
	.probe = tic_probe,
	.driver = {
		.name = "ptp-de25-tic",
		.of_match_table = tic_of_match,
	},
};
module_platform_driver(tic_driver);

MODULE_DESCRIPTION("PTP clock for the DE25-Nano FPGA time interval counter");
MODULE_LICENSE("GPL");
