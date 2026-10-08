# MDIO frame timestamps: series plan

This branch (`mdio-timestamp/work`) holds the net change for the series as ordinary commits made as
we go. This file says how to split that change into patches, and holds the text that goes into the
email. It is not part of any patch.

Base: net-next `45ad84d2800`. Open items: tracking page
https://claude.ai/artifact/QLFHJhSmeextZBi7rAAUBo

## Cover letter

Starting point: the description of raspberrypi/linux PR #7673 as it was on GitHub on
2026-10-08 (last edited 2026-10-06), converted to plain text. Not yet edited for netdev.

```
On a Raspberry Pi CM5, using PTP_SYS_OFFSET_EXTENDED to synchronize the
system clock from the BCM54210PE PHC results in the system clock being
about 32 µs ahead. On a CM4, this is reduced to about 7 µs. (The
difference is because the MDIO clock frequency is higher on the CM4:
10 MHz as opposed to 2.08 MHz on the CM5.) PTP_SYS_OFFSET_EXTENDED uses
gettimex64 to bracket each PHC read with system clock reads. With the
BCM54210PE PHY, these brackets include multiple MDIO transfers and are
very wide: about 218 µs on the CM5 and 43 µs on the CM4. This is using
rpi-6.18.y at 6.18.54. See below for measurement process.

This PR introduces a new MDIO operation that allows PHY drivers to ask
MDIO bus drivers to compute a bracket for the completion of an MDIO
write transfer. It then makes bcm-phy-ptp use this operation, and
implements it for both the macb and UniMAC drivers.

With these patches, the system clock synchronization error is reduced
from about 32 µs to below 0.1 µs on the CM5, and from about 7 µs to
below 1 µs on the CM4. PTP_SYS_OFFSET_EXTENDED brackets are reduced from
218 µs to about 2.2 µs on the CM5 and from 43 µs to 1.1 µs on the CM4.

This PR has four commits:

1. Introduce the new MDIO operation
2. Use it in the BCM54210PE PHY driver
3. Implement it for macb
4. Implement it for UniMAC

Some additional work should be done as follow-ups:

a. UniMAC should get the clock frequency from DT, but I don't know how
   to do this properly for the BCM2711.
b. macb should probably use a higher MDIO clock frequency: the CM4 runs
   at 10 MHz, but the CM5 runs at about 2.08 MHz.
c. The BCM54210PE PHY driver does not consistently check errors from
   MDIO operations.
d. The upstream macb MDIO polling interval is 1 µs, versus 100 µs in the
   Raspberry Pi tree, making PHC reads much slower. But the downstream
   change (commit 707efde3a98c) was done to reduce CPU wakeups, so it
   shouldn't simply be reverted.
e. The BCM54210PE PHY code is assuming that the time latches immediately
   on the completion of the MDIO write, but in practice there will be a
   small delay. If this is in the spec sheet somewhere or can be
   measured somehow, we could compensate for it.

I have done the patch in a way that I hope will be acceptable to
upstream. Note that SPI introduced a similar facility to allow a device
driver to obtain timestamp bounds from the bus controller in commit
79591b7db21d ("spi: Add a PTP system timestamp to the transfer
structure"). I plan to submit to upstream, but I would like to get some
feedback from Raspberry Pi maintainers on points (a) and (e) first.
Upstream might object to an MDIO operation with only a single PHY user,
but I surveyed other PHY drivers and identified at least two for which
this operation could be implemented: micrel (specifically LAN8814) and
dp83640, although they would need updating to use gettimex64 first.

Measurement process
-------------------

On the CM5, it is possible to measure the inaccuracy by taking advantage
of the recently introduced feature that allows selection of the PHC used
for packet timestamping. /dev/ptp0 is the BCM54210PE PHC; /dev/ptp1 is
the macb MAC PHC. PTP_SYS_OFFSET_EXTENDED brackets with macb are about
1 µs after upstream commit 9ca4ba242591 ("net: macb: fix ordering around
PTP timestamp read"). So we can use the following measurement approach:
synchronize /dev/ptp1 to an external PTP grandmaster; synchronize
/dev/ptp0 to a PPS from a GPS; synchronize the system clock from
/dev/ptp1 using phc2sys or chrony; then measure the offset between the
system clock and the result of PTP_SYS_OFFSET_EXTENDED.

An alternative way to check, which does not require a PTP grandmaster,
is to connect the same PPS signal to both the GPIO PPS pin (header pin
12) and to SYNC_OUT. Then have an NTP server discipline the system clock
using the GPS PPS signal, and as before measure the offset between the
system clock and the result of PTP_SYS_OFFSET_EXTENDED. But kernel PPS
has a significant bias (of the order of 10 µs). However, I have
developed a tool called ppsbias (https://github.com/jclark/ppsbias) to
measure this, which works by polling GPIO memory. If you configure the
NTP server with the offset from ppsbias, then you can get an estimate
that is accurate to within about 1 µs. The results from ppsbias are
consistent with results from using PTP with MAC PHC. On a CM4, this is
the only method available, since /dev/ptp1 is not available.
```

## Split

### 1/4 net: mdio: add timestamped write operation

- `include/linux/phy.h`: `write_sts` in `struct mii_bus`; `phy_supports_write_sts()`;
  `__phy_write_sts()`
- `include/linux/mdio.h`: declarations of `__mdiobus_write_sts()` and
  `mdiobus_supports_write_sts()`; `mdiobus_sts_bounds()` and its `math64.h`/`time64.h` includes
  (item 9: move these to 3/4)
- `drivers/net/phy/mdio_bus.c`: `__mdiobus_write_sts()`, `mdiobus_supports_write_sts()`

### 2/4 net: phy: broadcom: use timestamped MDIO writes in gettimex64

- `drivers/net/phy/bcm-phy-lib.c`, `bcm-phy-lib.h`: `bcm_phy_supports_write_exp_sts()`,
  `__bcm_phy_write_exp_sts()`, `bcm_phy_write_exp_sts()`
- `drivers/net/phy/bcm-phy-ptp.c`: `bcm_ptp_framesync_sts()`, `bcm_ptp_framesync_ts()`

### 3/4 net: macb: implement timestamped MDIO writes

- `drivers/net/ethernet/cadence/macb.h`, `macb_main.c`

### 4/4 net: mdio: bcm-unimac: implement timestamped MDIO writes

- `drivers/net/mdio/mdio-bcm-unimac.c`

## Commit messages

### 1/4

```
net: mdio: add timestamped write operation

Add an optional write_sts bus operation returning system timestamp
bounds for completion of an MDIO write. This allows PHY drivers to
obtain tighter bounds when implementing gettimex64. SPI introduced a
similar facility to allow a device driver to obtain timestamp bounds
from the bus controller in commit 79591b7db21d ("spi: Add a PTP system
timestamp to the transfer structure").

A PHY driver using this operation is responsible for fallback if the bus
driver does not implement it.

Signed-off-by: James Clark <jjc@jclark.com>
Assisted-by: LLM
```

### 2/4

```
net: phy: broadcom: use timestamped MDIO writes in gettimex64

Use __phy_write_sts() to obtain tighter system timestamp bounds in
gettimex64 by timestamping completion of the write that triggers
capture of the PHC time.

The existing framesync path ignores MDIO write errors and this patch
leaves that unchanged.

Signed-off-by: James Clark <jjc@jclark.com>
Assisted-by: LLM
```

### 3/4

```
net: macb: implement timestamped MDIO writes

Implement write_sts to provide system timestamp bounds for completion
of an MDIO write. This enables tighter system timestamp bounds in PHY
implementations of gettimex64.

Take system timestamps around the command register write, then add
bounds on the transfer time calculated from the peripheral clock rate
and configured MDC divider.

Signed-off-by: James Clark <jjc@jclark.com>
Assisted-by: LLM
```

### 4/4

```
net: mdio: bcm-unimac: implement timestamped MDIO writes

Implement write_sts to provide system timestamp bounds for completion
of an MDIO write. This enables tighter system timestamp bounds in PHY
implementations of gettimex64.

Take system timestamps around the command start, then add bounds on
the transfer time calculated from the reference clock rate and
configured MDC divider.

Use a 200 MHz reference rate for BCM2711 GENET, whose clock is not
described in DT.

Signed-off-by: James Clark <jjc@jclark.com>
Assisted-by: LLM
```
