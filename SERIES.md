# MDIO frame timestamps: series plan

This branch (`mdio-timestamp/work`) holds the net change for the series as ordinary commits made as
we go. This file says how to split that change into patches, and holds the text that goes into the
email. It is not part of any patch.

Base: net-next `45ad84d2800`. Open items: tracking page
https://claude.ai/artifact/QLFHJhSmeextZBi7rAAUBo

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

## Cover letter

Not written yet. Start from `pr-message.md` in the project directory.

```
```
