# enet_min: minimal bare-metal CPSW and ICSSG Ethernet drivers (AM243x)

Two self-contained drivers, one `.c` + one `.h` each, plus an echo example
for each. No Enet LLD, no UDMA LLD, no PRUICSS driver, no RTOS and no
SysConfig Ethernet module. The only SDK pieces used are the ones a
nortos `hello_world` already links: Sciclient (resource and power
management through the TISCI firmware), CacheP/ClockP, Pinmux and DebugP.

| File | What it is |
|---|---|
| `cpsw_min.c/.h` | CPSW3G: ALE bypass, host port, MAC ports 1/2, DP83867 or DP83869 PHY (by PHY ID), MDIO manual mode |
| `cpsw_min_cpdma.c` | Same API on the AM261x CPSW3G with CPDMA instead of PKTDMA, fixed-link MII, no MDIO (see below) |
| `cpsw_min_cpdma_regs.h`, `cpsw_min_cpdma_csl_check.c` | AM261x register map, and a build-time check of it against the Enet LLD CSL headers |
| `icssg_min.c/.h` | ICSSG dual-MAC firmware load/config, one slice per object, DP83869 PHY |
| `cpsw_min_example.c` | CPSW port 1 echo + 1 s broadcast test frame |
| `icssg_min_example.c` | ICSSG1 port 1 echo + 1 s broadcast test frame |
| `{cpsw,icssg}/am243x-{evm,lp}/r5fss0-0_nortos` | Projects (hello_world SysConfig: UART log, MPU, clocks only) |
| `{cpsw,cpsw_lean}/am261x-lp/r5fss0-0_nortos` | LP-AM261 projects (hello_world SysConfig plus I2C0, an uncached CPPI descriptor window) |
| `min_log.c/.h` | Polled UART0 console with an integer-only formatter; replaces the DPL log backend |
| `{cpsw,icssg}_lean/am243x-{evm,lp}/r5fss0-0_nortos` | Same examples without the SysConfig UART/CCS log, printf or stdio; 1 KB heap, 4 KB stack |
| `hello_lean.c`, `hello_lean/am243x-evm/r5fss0-0_nortos` | nortos hello_world on `min_log.c` (no UART driver, printf or stdio): the SDK floor the lean examples build on |

## Boards

| | AM243x EVM | AM243x LP (`-DENET_MIN_BOARD_AM243X_LP`) |
|---|---|---|
| CPSW port 1 | DP83867, MDIO address 0 | DP83869 on the RGMII1 jack, MDIO address 3 |
| CPSW MDIO pins | PRG0_PRU1_GPO19/18 | PRG1_MDIO0_MDC/MDIO, shared with PRU_ICSSG1 |
| CPSW extras | | E3 and later: GPIO0_26 PHY reset and GPIO0_27 RGMII1 mux select, both driven high. E1/E2: GPIO0_70 mux select driven high, PHY reset (GPIO0_73) left alone. The revision is read from the board ID EEPROM at start-up |
| ICSSG1 port 1 | DP83869, MDIO address 15 | DP83869 on jack J18, MDIO address 15 |
| ICSSG PHY delay | TX 0.75 ns, RX 2.0 ns | TX 0.5 ns, RX 0.5 ns (`IcssgMin_Cfg.phyDelay`) |

The LP projects differ from the EVM ones only in the SysConfig package
(ALX), the UART0 balls and the missing DDR MPU region. The RGMII1 pins of
both CPSW and ICSSG1 are the same on both boards.

## Design

* PKTDMA: one TX channel, one RX channel and flow, 8 TX + 16 RX host
  descriptors (128 B) and 1536 B buffers, all embedded in one driver object
  in cached MSRAM. Rings run in dual-ring mode; cache maintenance is
  explicit.
* Zero copy (CPSW): `CpswMin_sendRx()` transmits a received buffer edited
  in place. The TX descriptor borrows the RX buffer, and the buffer goes
  back to RX when that TX completes. TX completions are reclaimed by
  `CpswMin_recv()` when no frame is pending, which keeps the RX buffer
  invalidate off the echo path. The CPSW echo examples use it; build with
  `-DCPSW_MIN_ECHO_COPY` for the copying echo.
* `-DCPSW_MIN_TESTER` (cpsw_min_example.c) turns the example into an echo tester: 2 s after link up it runs the probe, RTT and throughput tests of the SOEM `echo_test.c` against a far-end echo and prints the same `ECHO ...` lines (`ECHO RESULT PASS/FAIL`), then idles.
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

## AM261x (LP-AM261, `-DENET_MIN_BOARD_AM261X_LP`)

`cpsw_min_cpdma.c` implements the same `CpswMin_*` API on the AM261x CPSW3G,
which moves packets with the CPDMA instead of the PKTDMA: one TX and one RX
channel (channel 0) over linked CPPI 3.0 descriptors, polled. There is no
Sciclient. Differences from the AM243x driver:

* MAC port 2 is a fixed 100 Mb/s full-duplex MII link
  (`CpswMin_PortCfg.fixedSpeed`); MDIO is never touched (the CPSW MDC pad is
  not level-shifted on the LP-AM261 RevE2) and `CpswMin_poll()` always reports
  link up. The DP83826E add-on card autonegotiates by itself.
* The descriptors are in the uncached `.bss:ENET_CPPI_DESC` section (an 8 KB
  CPPI_DESC memory region plus a NonCached MPU region in the project
  SysConfig) because the CPDMA updates them in place. Packet buffers stay in
  cached OCRAM with explicit cache maintenance.
* The example drives the Eth Conn 0 pads GPIO87..102 as MII2, returns the two
  IO expander pins that route MDIO to their board defaults over I2C0, and
  logs U42. See `AM261X-PORT.md` for the design, the verified register list
  and the hardware status.

Build with the AM261x SDK, for example
`gmake cpsw_min_example.release.out MCU_PLUS_SDK_PATH=<sdk>` in
`cpsw/am261x-lp/r5fss0-0_nortos/ti-arm-clang`.

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

| Image | ICSSG1 (J18) | CPSW (RGMII1) |
|---|---|---|
| lean | 12/12 pass; 60 B RTT 36 us avg; 1514 B 98.2 Mb/s | 12/12 pass; zero-copy echo: 60 B RTT 33.2 us avg (p99 34), 1514 B RTT 316.5 us, 98.2 Mb/s |
| standard | 12/12 pass after both fixes below | 12/12 pass after the log fix |

Standard-image loss, now fixed: the SDK UART log blocks until the line is
out. The 5 s counter print held the poll loop for about 410 us, the RX
ring filled and the PKTDMA dropped frames (RX channel DCNT). The examples
now print counters only after 1 s without RX. On CPSW the corrupted
echoes went away with the drops.

ICSSG corrupted echoes, now fixed: 1514 B echoes came back with zeroed
64 B runs. The firmware host pool, egress queues and scratch sit in cached
.bss, and start-up zeroing left dirty D-cache lines over them that were
later evicted onto frame data. The driver now writes back and invalidates
that region before the firmware owns it.

Latency split (`-DCPSW_MIN_TIMING`: CPTS timestamps at the MAC plus R5F
cycle counters), LP-AM243, 60 B, zero copy: the frame spends 11.8 us in
the board. That is 5.1 us receiving the frame (store and forward), 0.9 us
recv, 0.1 us MAC swap, 0.65 us send, and about 5 us of RX DMA, poll
detection and TX DMA. TI's enet_layer2_cpsw (interrupt + task) on the
same bench: 60 B RTT 43.1 us; it drops 1514 B frames.

Board notes:
* LP-AM243 E1/E2 select the RGMII1 mux with GPIO0_70. The SDK LP examples
  drive GPIO0_27 (the E3 pin, a user LED on E1/E2), so on those boards
  the PHY links but no CPSW frame reaches it. The CPSW example reads the
  revision from the board ID EEPROM (I2C0 0x50, offset 0x1A, e.g. "E1")
  at start-up and picks the pins; `-DCPSW_MIN_LP_REV_E2` only selects
  E1/E2 when the EEPROM cannot be read.
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
