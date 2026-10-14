# bcm-phy-ptp deadlock: series plan

This branch (`bcm-phy-ptp-deadlock/work`) holds the code for the series as ordinary commits made
as we go. This file says how to split that code into patches, and holds the text that goes into
the email. It is not part of any patch. `tools/make-series.py` builds
`bcm-phy-ptp-deadlock/series` from the two; `tools/make-patches.py` generates the patch files.

Base: net `8e2fbe0e75d`. Target: net, both patches `Cc: stable`.

## Cover letter

Subject (git format-patch adds `[PATCH net 0/2]`):

```
TBD
```

Draft for netdev (in progress):

```
TBD
```

## Split

### 1/2 ptp: hold pincfg_mux around ->enable() in the sysfs attributes

- `drivers/ptp/ptp_sysfs.c`

### 2/2 net: phy: broadcom: fix deadlock when disabling EXTTS or PEROUT

- `drivers/net/phy/bcm-phy-ptp.c`

## Commit messages

### 1/2

```
ptp: hold pincfg_mux around ->enable() in the sysfs attributes

The ptp core holds ptp->pincfg_mux around the ->enable() calls for the
PTP_EXTTS_REQUEST, PTP_PEROUT_REQUEST and PTP_ENABLE_PPS ioctls, and
around the disables done by PTP_PIN_SETFUNC and ptp_clock_unregister().
The kernel-doc of ptp_find_pin() tells drivers that they may rely on
this and call ptp_find_pin(), which needs the mutex, from ->enable().

The sysfs attributes extts_enable, period and pps_enable call
->enable() without the mutex.  Commit 62582a7ee783 ("ptp: Avoid
deadlocks in the programmable pin code.") moved the locking from
ptp_find_pin() to the ioctl callers of ->enable(), but did not change
ptp_sysfs.c.  So an ->enable() call from sysfs can run at the same
time as one from an ioctl or PTP_PIN_SETFUNC, and a driver's
ptp_find_pin() call reads the pin table without the lock.

Take pincfg_mux around ->enable() in the three sysfs store functions,
as the ioctls do.

Fixes: 62582a7ee783 ("ptp: Avoid deadlocks in the programmable pin code.")
Cc: stable@vger.kernel.org
Assisted-by: LLM
```

### 2/2

```
net: phy: broadcom: fix deadlock when disabling EXTTS or PEROUT

bcm_ptp_enable() holds priv->mutex while it calls
cancel_delayed_work_sync() on pin_work, when it disables EXTTS or
PEROUT and when it re-enables one that is already active.  The work
functions, bcm_ptp_extts_work() and bcm_ptp_perout_work(), start by
taking priv->mutex.  If the work starts running after bcm_ptp_enable()
has taken the mutex but before the cancel, it blocks on the mutex and
cancel_delayed_work_sync() waits for it forever.  Both waits are
uninterruptible.  So every later user of priv->mutex then hangs too:
gettime, settime, adjtime, adjfine and TX timestamp retrieval in the
PTP aux worker.

The EXTTS work runs every 250 ms and the window is a few MDIO
accesses, so this is rare, but it has been seen: a PTP_EXTTS_REQUEST
that disabled EXTTS on a CM5's BCM54210PE never returned, and the
hung-task detector reported the ioctl in cancel_delayed_work_sync(),
the EXTTS work and the PTP aux worker blocked on the mutex.

Cancel the work synchronously only after releasing the mutex.  The
work then either sees pin_active clear and returns, or has already
passed that check, in which case the cancel stops it from requeueing
itself.  The ptp core holds ptp->pincfg_mux around every ->enable()
call (for the sysfs attributes, since the previous patch), so no other
enable request can run between the unlock and the cancel.

When re-enabling a function that is already active, cancel the
pending work without waiting and schedule it again.  The work
function is the same, because the ptp core disables a pin's function
before changing it.  Initialise the work only when no function is
active; the work is then neither pending nor running.

bcm_ptp_stop() runs without the mutex, so it keeps the synchronous
cancel.  bcm_ptp_cancel_func() no longer cancels the work, so rename it
bcm_ptp_disable_func().

Fixes: 7bfe91efd525 ("net: phy: Add support for 1PPS out and external timestamps")
Cc: stable@vger.kernel.org # needs "ptp: hold pincfg_mux around ->enable() in the sysfs attributes"
Assisted-by: LLM
```
