# enet_min: minimal bare-metal CPSW and ICSSG Ethernet drivers (AM243x)

Two self-contained drivers, one `.c` + one `.h` each, plus an echo example
for each. No Enet LLD, no UDMA LLD, no PRUICSS driver, no RTOS and no
SysConfig Ethernet module. The only SDK pieces used are the ones a
nortos `hello_world` already links: Sciclient (resource and power
management through the TISCI firmware), CacheP/ClockP, Pinmux and DebugP.

| File | What it is |
|---|---|
| `cpsw_min.c/.h` | CPSW3G: ALE bypass, host port, MAC ports 1/2, DP83867 or DP83869 PHY (by PHY ID), MDIO manual mode |
| `icssg_min.c/.h` | ICSSG dual-MAC firmware load/config, one slice per object, DP83869 PHY |
| `cpsw_min_example.c` | CPSW port 1 echo + 1 s broadcast test frame |
| `icssg_min_example.c` | ICSSG1 port 1 echo + 1 s broadcast test frame |
| `{cpsw,icssg}/am243x-{evm,lp}/r5fss0-0_nortos` | Projects (hello_world SysConfig: UART log, MPU, clocks only) |
| `min_log.c/.h` | Polled UART0 console with an integer-only formatter; replaces the DPL log backend |
| `{cpsw,icssg}_lean/am243x-{evm,lp}/r5fss0-0_nortos` | Same examples without the SysConfig UART/CCS log, printf or stdio; 1 KB heap, 4 KB stack |
| `hello_lean.c`, `hello_lean/am243x-evm/r5fss0-0_nortos` | nortos hello_world on `min_log.c` (no UART driver, printf or stdio): the SDK floor the lean examples build on |

## Boards

| | AM243x EVM | AM243x LP (`-DENET_MIN_BOARD_AM243X_LP`) |
|---|---|---|
| CPSW port 1 | DP83867, MDIO address 0 | DP83869 on the RGMII1 jack, MDIO address 3 |
| CPSW MDIO pins | PRG0_PRU1_GPO19/18 | PRG1_MDIO0_MDC/MDIO, shared with PRU_ICSSG1 |
| CPSW extras | | E3 and later: GPIO0_26 PHY reset and GPIO0_27 RGMII1 mux select, both driven high. E1/E2 (`-DCPSW_MIN_LP_REV_E2`): GPIO0_70 mux select driven high, PHY reset (GPIO0_73) left alone |
| ICSSG1 port 1 | DP83869, MDIO address 15 | DP83869 on jack J18, MDIO address 15 |
| ICSSG PHY delay | TX 0.75 ns, RX 2.0 ns | TX 0.5 ns, RX 0.5 ns (`IcssgMin_Cfg.phyDelay`) |

The LP projects differ from the EVM ones only in the SysConfig package
(ALX), the UART0 balls and the missing DDR MPU region. The RGMII1 pins of
both CPSW and ICSSG1 are the same on both boards.

## Design

* PKTDMA: one TX channel, one RX channel and flow, 8 + 8 host descriptors
  (128 B) and 1536 B buffers, all embedded in one driver object in cached
  MSRAM. Rings run in dual-ring mode; cache maintenance is explicit.
* Rings, channels, flows and PSI-L pairing are set up with the SDK
  Sciclient RM calls, so the resources must be assigned to r5fss0-0 in
  the board configuration (they are by default).
* Polled only: call `*_poll()` every ~100 ms for the PHY link state and
  `*_recv()` as often as needed. Zero-copy buffer API.
* MDIO runs in manual (bit-bang) mode, as the Enet examples do for
  erratum i2329.
* Pinmux is a `Pinmux_PerCfg_t` table in each example (values from the
  Enet SysConfig output for the AM243x EVM).
* ICSSG: one host queue (QoS 1, classifiers off, PCP regenerated to 0),
  so the firmware needs 8 KB host pool + 18 KB host queue + 2 KB scratch
  per port. `ICSSG_MIN_SLICES=1` links only slice 0 firmware (~14 KB).
* Multi-instance: CPSW takes up to two MAC ports in `CpswMin_Cfg.port[]`;
  ICSSG takes one object per slice (ICSSG0 = one more row in the SoC
  resource table in `icssg_min.c`).

## Lean variants

The `*_lean` projects drop the SysConfig UART console (which links the
UART driver, UDMA and Sciclient IRQ routing even with DMA off) and the
SDK printf. `min_log.c` defines `_DebugP_logZone`/`_DebugP_log`, so
`DebugP_log()` and library asserts print through a polled UART0 at
115200 8N1 using a ~0.5 KB integer-only formatter (%c %s %d %u %x %p).

## Build

```
gmake -C cpsw/am243x-evm/r5fss0-0_nortos/ti-arm-clang all
gmake -C icssg/am243x-evm/r5fss0-0_nortos/ti-arm-clang all
gmake -C cpsw_lean/am243x-evm/r5fss0-0_nortos/ti-arm-clang all
gmake -C icssg_lean/am243x-evm/r5fss0-0_nortos/ti-arm-clang all
gmake -C hello_lean/am243x-evm/r5fss0-0_nortos/ti-arm-clang all
```

Use `am243x-lp` in place of `am243x-evm` for the LP projects.

The makefiles expect this repo at `source/networking/enet/core` inside the
MCU+ SDK, like the other Enet examples.

## Status

Tested on an LP-AM243 (revision E1) against an LP-AM261 running the SOEM
`x` echo tester over CPSW, 100 Mb/s full duplex. Not yet tested on the EVM.

| Image | ICSSG1 (J18) | CPSW (RGMII1, `-DCPSW_MIN_LP_REV_E2`) |
|---|---|---|
| lean | 12/12 pass; 60 B RTT 36 us avg; 1514 B 98.2 Mb/s | 12/12 pass; 60 B RTT 35.8 us avg; 1514 B 98.2 Mb/s |
| standard | 7/12: frames lost and some corrupted under throughput load | 6/12: same pattern |

The standard images fail the same way with both drivers, so the cause is
in the shared example loop rather than in the drivers. Suspected, not yet
confirmed: the 5 s console print blocks the poll loop and the RX ring
overflows; a received buffer is reused before its echo has been sent.

Board notes:
* LP-AM243 E1/E2 select the RGMII1 mux with GPIO0_70. The SDK LP examples
  drive GPIO0_27 (the E3 pin, a user LED on E1/E2), so on those boards
  the PHY links but no CPSW frame reaches it. Build with
  `-DCPSW_MIN_LP_REV_E2`. The revision is in the board ID EEPROM (I2C0
  0x50, offset 0x1A).
* On E1/E2, do not pulse the PHY reset (GPIO0_73) with the SoC pads
  attached: the DP83869 re-samples its straps and can come back
  advertising 1000 Mb/s only.
* A JTAG system reset does not reset MAIN GPIO, pad configuration or the
  PHYs. The ICSSG driver power-cycles the ICSSG module at start so that a
  reload behaves like a cold boot.

Open items: the ICSSG core clock is left at the boot default (set
`ICSSG_MIN_CORE_CLK_HZ` to force one), the VBUSM QoS priority settings done
by the Enet LLD are skipped, and the LP board revision is a build option
rather than read from the EEPROM.
