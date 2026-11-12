// SPDX-License-Identifier: GPL-2.0-only
/*******************************************************************************
  PTP 1588 clock using the STMMAC.

  Copyright (C) 2013  Vayavya Labs Pvt Ltd


  Author: Rayagond Kokatanur <rayagond@vayavyalabs.com>
*******************************************************************************/
#include "stmmac.h"
#include "stmmac_est.h"
#include "stmmac_ptp.h"

#define PTP_SAFE_TIME_OFFSET_NS	500000

/**
 * stmmac_adjust_freq
 *
 * @ptp: pointer to ptp_clock_info structure
 * @scaled_ppm: desired period change in scaled parts per million
 *
 * Description: this function will adjust the frequency of hardware clock.
 *
 * Scaled parts per million is ppm with a 16-bit binary fractional field.
 */
static int stmmac_adjust_freq(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct stmmac_priv *priv =
	    container_of(ptp, struct stmmac_priv, ptp_clock_ops);
	unsigned long flags;
	u32 addend;

	addend = adjust_by_scaled_ppm(priv->default_addend, scaled_ppm);

	write_lock_irqsave(&priv->ptp_lock, flags);
	stmmac_config_addend(priv, priv->ptpaddr, addend);
	write_unlock_irqrestore(&priv->ptp_lock, flags);

	return 0;
}

/**
 * stmmac_adjust_time
 *
 * @ptp: pointer to ptp_clock_info structure
 * @delta: desired change in nanoseconds
 *
 * Description: this function will shift/adjust the hardware clock time.
 */
static int stmmac_adjust_time(struct ptp_clock_info *ptp, s64 delta)
{
	struct stmmac_priv *priv =
	    container_of(ptp, struct stmmac_priv, ptp_clock_ops);
	unsigned long flags;
	u32 sec, nsec;
	u32 quotient, reminder;
	int neg_adj = 0;
	bool xmac;

	xmac = dwmac_is_xmac(priv->plat->core_type);

	if (delta < 0) {
		neg_adj = 1;
		delta = -delta;
	}

	quotient = div_u64_rem(delta, 1000000000ULL, &reminder);
	sec = quotient;
	nsec = reminder;

	/* If EST is enabled, disabled it before adjust ptp time. */
	mutex_lock(&priv->est_lock);
	if (priv->est && priv->est->enable)
		stmmac_est_configure(priv, priv, priv->est,
				     priv->plat->clk_ptp_rate, false);
	mutex_unlock(&priv->est_lock);

	write_lock_irqsave(&priv->ptp_lock, flags);
	stmmac_adjust_systime(priv, priv->ptpaddr, sec, nsec, neg_adj, xmac);
	write_unlock_irqrestore(&priv->ptp_lock, flags);

	/* Calculate new basetime and re-configured EST after PTP time adjust. */
	mutex_lock(&priv->est_lock);
	if (priv->est && priv->est->enable)
		__stmmac_setup_est(priv);
	mutex_unlock(&priv->est_lock);

	return 0;
}

/**
 * stmmac_get_timex - read the PTP clock, with system timestamps if requested
 * @ptp: pointer to ptp_clock_info structure
 * @ts: pointer to hold time/result
 * @sts: pointer to hold system timestamps around the read, or NULL
 *
 * Return: 0
 */
static int stmmac_get_timex(struct ptp_clock_info *ptp, struct timespec64 *ts,
			    struct ptp_system_timestamp *sts)
{
	struct stmmac_priv *priv =
	    container_of(ptp, struct stmmac_priv, ptp_clock_ops);
	unsigned long flags;
	u64 ns = 0;

	read_lock_irqsave(&priv->ptp_lock, flags);
	stmmac_get_systime(priv, priv->ptpaddr, &ns, sts);
	read_unlock_irqrestore(&priv->ptp_lock, flags);

	*ts = ns_to_timespec64(ns);

	return 0;
}

/**
 * stmmac_set_time
 *
 * @ptp: pointer to ptp_clock_info structure
 * @ts: time value to set
 *
 * Description: this function will set the current time on the
 * hardware clock.
 */
static int stmmac_set_time(struct ptp_clock_info *ptp,
			   const struct timespec64 *ts)
{
	struct stmmac_priv *priv =
	    container_of(ptp, struct stmmac_priv, ptp_clock_ops);
	unsigned long flags;

	write_lock_irqsave(&priv->ptp_lock, flags);
	stmmac_init_systime(priv, priv->ptpaddr, ts->tv_sec, ts->tv_nsec);
	write_unlock_irqrestore(&priv->ptp_lock, flags);

	return 0;
}

/* Start or stop reporting auxiliary snapshots as external timestamp events.
 * On XGMAC, whose reporting code holds ts_status_lock, no snapshot is read
 * from the FIFO or reported once this has stopped it. Called with aux_ts_lock
 * held.
 */
static void stmmac_extts_report(struct stmmac_priv *priv, unsigned int index,
				bool on)
{
	unsigned long flags;

	lockdep_assert_held(&priv->aux_ts_lock);

	spin_lock_irqsave(&priv->hw->ts_status_lock, flags);
	if (on) {
		priv->extts_index = index;
		priv->plat->flags |= STMMAC_FLAG_EXT_SNAPSHOT_EN;
	} else {
		priv->plat->flags &= ~STMMAC_FLAG_EXT_SNAPSHOT_EN;
	}
	stmmac_aux_snapshot_irq(priv, priv, on && priv->mac_irq_ready);
	spin_unlock_irqrestore(&priv->hw->ts_status_lock, flags);
}

/**
 * stmmac_ptp_mac_irq_ready - the MAC interrupt handler was installed or goes
 * @priv: driver private structure
 * @ready: the handler is installed
 * Description: on a core that enables an interrupt for auxiliary snapshots
 * only while an external timestamp channel is enabled, keep it disabled
 * while there is no handler for it. A snapshot taken meanwhile stays in the
 * FIFO, and is reported when the interrupt is enabled.
 */
void stmmac_ptp_mac_irq_ready(struct stmmac_priv *priv, bool ready)
{
	unsigned long flags;
	bool on;

	spin_lock_irqsave(&priv->hw->ts_status_lock, flags);
	priv->mac_irq_ready = ready;
	on = ready && (priv->plat->flags & STMMAC_FLAG_EXT_SNAPSHOT_EN);
	stmmac_aux_snapshot_irq(priv, priv, on);
	spin_unlock_irqrestore(&priv->hw->ts_status_lock, flags);
}

/* Enable or disable an auxiliary snapshot. Called with aux_ts_lock held.
 *
 * The order matters. Nothing may read the FIFO while it is cleared, so stop
 * the reporting first. Clear the FIFO with every channel disabled, start the
 * reporting, and only then enable the channel: no snapshot can then be taken
 * before it would be reported.
 */
static int stmmac_extts_set(struct stmmac_priv *priv, unsigned int index,
			    bool on)
{
	void __iomem *ptpaddr = priv->ptpaddr;
	u32 acr_value;
	int ret;

	lockdep_assert_held(&priv->aux_ts_lock);

	stmmac_extts_report(priv, index, false);

	acr_value = readl(ptpaddr + PTP_ACR);
	acr_value &= ~PTP_ACR_MASK;
	if (on)
		acr_value |= PTP_ACR_ATSFC;
	writel(acr_value, ptpaddr + PTP_ACR);

	if (!on)
		return 0;

	/* wait for auxts fifo clear to finish */
	ret = readl_poll_timeout(ptpaddr + PTP_ACR, acr_value,
				 !(acr_value & PTP_ACR_ATSFC), 10, 10000);
	if (ret)
		return ret;

	stmmac_extts_report(priv, index, true);

	/* Enable External snapshot trigger */
	acr_value = readl(ptpaddr + PTP_ACR);
	acr_value |= PTP_ACR_ATSEN(index);
	writel(acr_value, ptpaddr + PTP_ACR);

	return 0;
}

static int stmmac_enable(struct ptp_clock_info *ptp,
			 struct ptp_clock_request *rq, int on)
{
	struct stmmac_priv *priv =
	    container_of(ptp, struct stmmac_priv, ptp_clock_ops);
	struct stmmac_pps_cfg *cfg;
	int ret = -EOPNOTSUPP;
	unsigned long flags;

	switch (rq->type) {
	case PTP_CLK_REQ_PEROUT: {
		struct timespec64 curr_time;
		u64 target_ns = 0;
		u64 ns = 0;

		/* Reject requests with unsupported flags */
		if (rq->perout.flags)
			return -EOPNOTSUPP;

		cfg = &priv->pps[rq->perout.index];

		cfg->start.tv_sec = rq->perout.start.sec;
		cfg->start.tv_nsec = rq->perout.start.nsec;

		/* A time set in the past won't trigger the start of the flexible PPS generation for
		 * the GMAC5. For some reason it does for the GMAC4 but setting a time in the past
		 * should be addressed anyway. Therefore, any value set it the past is considered as
		 * an offset compared to the current MAC system time.
		 * Be aware that an offset too low may not trigger flexible PPS generation
		 * if time spent in this configuration makes the targeted time already outdated.
		 * To address this, add a safe time offset.
		 */
		if (!cfg->start.tv_sec && cfg->start.tv_nsec < PTP_SAFE_TIME_OFFSET_NS)
			cfg->start.tv_nsec += PTP_SAFE_TIME_OFFSET_NS;

		target_ns = cfg->start.tv_nsec + ((u64)cfg->start.tv_sec * NSEC_PER_SEC);

		stmmac_get_systime(priv, priv->ptpaddr, &ns, NULL);
		if (ns > TIME64_MAX - PTP_SAFE_TIME_OFFSET_NS)
			return -EINVAL;

		curr_time = ns_to_timespec64(ns);
		if (target_ns < ns + PTP_SAFE_TIME_OFFSET_NS) {
			cfg->start = timespec64_add_safe(cfg->start, curr_time);
			if (cfg->start.tv_sec == TIME64_MAX)
				return -EINVAL;
		}

		cfg->period.tv_sec = rq->perout.period.sec;
		cfg->period.tv_nsec = rq->perout.period.nsec;

		write_lock_irqsave(&priv->ptp_lock, flags);
		ret = stmmac_flex_pps_config(priv, priv->ioaddr,
					     rq->perout.index, cfg, on,
					     priv->sub_second_inc,
					     priv->systime_flags);
		write_unlock_irqrestore(&priv->ptp_lock, flags);
		break;
	}
	case PTP_CLK_REQ_EXTTS: {
		bool enabled;

		/* The clock is visible to user space before
		 * ptp_clock_register() returns, and events need the clock.
		 */
		if (!READ_ONCE(priv->ptp_clock))
			return -EBUSY;

		mutex_lock(&priv->aux_ts_lock);
		enabled = priv->plat->flags & STMMAC_FLAG_EXT_SNAPSHOT_EN;

		/* Only one auxiliary snapshot can be enabled at a time. The
		 * enable bits of PTP_ACR do not tell: a cross-timestamp may
		 * have left the bit of its internal snapshot set.
		 */
		if (on && enabled && priv->extts_index != rq->extts.index) {
			netdev_err(priv->dev,
				   "Cannot enable auxiliary snapshot %u as auxiliary snapshot %u is already enabled\n",
				   rq->extts.index, priv->extts_index);
			mutex_unlock(&priv->aux_ts_lock);
			return -EBUSY;
		}

		/* Enabling the channel that is enabled, or disabling one that
		 * is not, changes nothing.
		 */
		if (on ? enabled :
		    (!enabled || priv->extts_index != rq->extts.index)) {
			mutex_unlock(&priv->aux_ts_lock);
			return 0;
		}

		netdev_dbg(priv->dev, "Auxiliary Snapshot %u %s.\n",
			   rq->extts.index, on ? "enabled" : "disabled");
		ret = stmmac_extts_set(priv, rq->extts.index, on);
		mutex_unlock(&priv->aux_ts_lock);
		break;
	}

	default:
		break;
	}

	return ret;
}

/**
 * stmmac_get_syncdevicetime
 * @device: current device time
 * @system: system counter value read synchronously with device time
 * @ctx: context provided by timekeeping code
 * Description: Read device and system clock simultaneously and return the
 * corrected clock values in ns.
 **/
static int stmmac_get_syncdevicetime(ktime_t *device,
				     struct system_counterval_t *system,
				     void *ctx)
{
	struct stmmac_priv *priv = (struct stmmac_priv *)ctx;

	return priv->plat->crosststamp(device, system, ctx);
}

static int stmmac_getcrosststamp(struct ptp_clock_info *ptp,
				 struct system_device_crosststamp *xtstamp)
{
	struct stmmac_priv *priv =
		container_of(ptp, struct stmmac_priv, ptp_clock_ops);

	return get_device_system_crosststamp(stmmac_get_syncdevicetime,
					     priv, NULL, xtstamp);
}

/* structure describing a PTP hardware clock */
const struct ptp_clock_info stmmac_ptp_clock_ops = {
	.owner = THIS_MODULE,
	.name = "stmmac ptp",
	.max_adj = 62500000,
	.n_alarm = 0,
	.n_ext_ts = 0, /* will be overwritten in stmmac_ptp_register */
	.n_per_out = 0, /* will be overwritten in stmmac_ptp_register */
	.n_pins = 0,
	.pps = 0,
	.adjfine = stmmac_adjust_freq,
	.adjtime = stmmac_adjust_time,
	.gettimex64 = stmmac_get_timex,
	.settime64 = stmmac_set_time,
	.enable = stmmac_enable,
};

/* structure describing a PTP hardware clock */
const struct ptp_clock_info dwmac1000_ptp_clock_ops = {
	.owner = THIS_MODULE,
	.name = "stmmac ptp",
	.max_adj = 62500000,
	.n_alarm = 0,
	.n_ext_ts = 1,
	.n_per_out = 0,
	.n_pins = 0,
	.pps = 0,
	.adjfine = stmmac_adjust_freq,
	.adjtime = stmmac_adjust_time,
	.gettimex64 = stmmac_get_timex,
	.settime64 = stmmac_set_time,
	.enable = dwmac1000_ptp_enable,
};

/**
 * stmmac_ptp_register
 * @priv: driver private structure
 * Description: this function will register the ptp clock driver
 * to kernel. It also does some house keeping work.
 */
void stmmac_ptp_register(struct stmmac_priv *priv)
{
	unsigned int pps_out_num = priv->dma_cap.pps_out_num;
	struct ptp_clock *ptp_clock;
	unsigned int n_ext_ts;

	if (pps_out_num > STMMAC_PPS_MAX) {
		dev_warn(priv->device,
			 "pps outputs (%u) exceeds driver maximum, limiting to %u\n",
			 pps_out_num, STMMAC_PPS_MAX);
		pps_out_num = STMMAC_PPS_MAX;
	}

	/* Calculate the clock domain crossing (CDC) error if necessary */
	priv->plat->cdc_error_adj = 0;
	if (priv->plat->core_type == DWMAC_CORE_GMAC4)
		priv->plat->cdc_error_adj = (2 * NSEC_PER_SEC) / priv->plat->clk_ptp_rate;

	/* Update the ptp clock parameters based on feature discovery, when
	 * available
	 */
	if (pps_out_num)
		priv->ptp_clock_ops.n_per_out = pps_out_num;

	n_ext_ts = priv->dma_cap.aux_snapshot_n;
	if (n_ext_ts)
		priv->ptp_clock_ops.n_ext_ts = n_ext_ts;

	if (priv->plat->ptp_max_adj)
		priv->ptp_clock_ops.max_adj = priv->plat->ptp_max_adj;

	if (priv->plat->crosststamp)
		priv->ptp_clock_ops.getcrosststamp = stmmac_getcrosststamp;

	mutex_init(&priv->aux_ts_lock);

	/* The clock is visible to user space before ptp_clock_register()
	 * returns. Never let it see an error pointer.
	 */
	ptp_clock = ptp_clock_register(&priv->ptp_clock_ops, priv->device);
	if (IS_ERR(ptp_clock)) {
		netdev_err(priv->dev, "ptp_clock_register failed\n");
		ptp_clock = NULL;
	}
	WRITE_ONCE(priv->ptp_clock, ptp_clock);

	if (priv->ptp_clock)
		netdev_info(priv->dev, "registered PTP clock\n");
	else
		mutex_destroy(&priv->aux_ts_lock);
}

/**
 * stmmac_ptp_restore_extts - enable the external timestamp channel again
 * @priv: driver private structure
 * @restore: the PTP clock works, so the channel can be enabled
 * Description: a reset of the MAC, as on resume, disables the auxiliary
 * snapshot and, on XGMAC, the timestamp interrupt. Enable them again if an
 * external timestamp channel was enabled, or forget the channel if that is
 * not possible.
 */
void stmmac_ptp_restore_extts(struct stmmac_priv *priv, bool restore)
{
	/* Only for the cores whose channels stmmac_enable() enables */
	if (!priv->ptp_clock || priv->ptp_clock_ops.enable != stmmac_enable)
		return;

	mutex_lock(&priv->aux_ts_lock);
	if (priv->plat->flags & STMMAC_FLAG_EXT_SNAPSHOT_EN) {
		if (!restore)
			stmmac_extts_report(priv, priv->extts_index, false);

		if (!restore ||
		    stmmac_extts_set(priv, priv->extts_index, true))
			netdev_warn(priv->dev,
				    "Auxiliary snapshot %u not enabled again\n",
				    priv->extts_index);
	}
	mutex_unlock(&priv->aux_ts_lock);
}

/**
 * stmmac_ptp_unregister
 * @priv: driver private structure
 * Description: this function will remove/unregister the ptp clock driver
 * from the kernel.
 */
void stmmac_ptp_unregister(struct stmmac_priv *priv)
{
	if (priv->ptp_clock) {
		/* The clock goes, and with it its enabled channel. Stop the
		 * events before the clock goes, and again afterwards in case
		 * a request enabled the channel in between.
		 */
		mutex_lock(&priv->aux_ts_lock);
		stmmac_extts_report(priv, priv->extts_index, false);
		mutex_unlock(&priv->aux_ts_lock);

		ptp_clock_unregister(priv->ptp_clock);

		mutex_lock(&priv->aux_ts_lock);
		stmmac_extts_report(priv, priv->extts_index, false);
		mutex_unlock(&priv->aux_ts_lock);
		WRITE_ONCE(priv->ptp_clock, NULL);
		pr_debug("Removed PTP HW clock successfully on %s\n",
			 priv->dev->name);
		mutex_destroy(&priv->aux_ts_lock);
	}
}
