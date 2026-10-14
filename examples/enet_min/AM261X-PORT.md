# enet_min on the AM261x (LP-AM261): CPDMA port of `cpsw_min` (Plane ECAT-38)

Status: builds clean. **Not run on hardware.** Everything below about
behaviour on the board is derived from the SDK sources and the SOEM example;
the hardware test must confirm it.

## What was implemented

* `cpsw_min_cpdma.c`: the `CpswMin_open/poll/getTxBuf/send/recv/recvDone`
  API on the AM261x CPSW3G, which uses the CPDMA instead of PKTDMA/UDMA.
  Polled, bare-metal nortos, no Sciclient, no Enet LLD. One TX and one RX
  channel (channel 0), 8 + 8 descriptors and 1536 B buffers (same
  `CPSW_MIN_NUM_TX/RX` knobs).
* `cpsw_min.h`: AM261x object layout under `#if defined(SOC_AM261X)`, and one
  new optional config field, `CpswMin_PortCfg.fixedSpeed`. The AM243x
  `cpsw_min.c` is untouched and its examples compile unchanged.
* `cpsw_min_cpdma_regs.h`: the register map; `cpsw_min_cpdma_csl_check.c`:
  `_Static_assert`s of every offset and bit against the Enet LLD CSL headers
  (compiled with `-I<sdk>/source/networking/enet/hw_include`, emits no code).
* `min_log.c/.h`: AM261x UART0 backend for the lean variant (`#if SOC_AM261X`,
  AM243x path unchanged).
* `cpsw_min_example.c`: `ENET_MIN_BOARD_AM261X_LP` branch: MAC port 2, MII2
  pinmux, expander release, U42 log. The echo is unchanged (`echo()` swaps
  the source MAC into the destination, puts our MAC as source, copies the
  rest) and the 1 s broadcast test frame goes out on port 2.
* Projects `cpsw/am261x-lp/r5fss0-0_nortos` and
  `cpsw_lean/am261x-lp/r5fss0-0_nortos` (SDK `hello_world` am261x-lp nortos
  makefile and SysConfig, plus I2C0, an 8 KB uncached CPPI descriptor region
  and a matching NonCached MPU region; lean has no UART driver and 1 KB heap
  and 4 KB stack like the AM243x lean projects).

## Design

### Link and ports
MAC port 2 is a fixed full-duplex MII link (`fixedSpeed =
CPSW_MIN_FIXED(CPSW_MIN_SPEED_100)`; 10 also accepted, PHY clocks set the real
speed, `MAC_CONTROL` is the same). MDIO is never touched: no MDIO register is
read or written and the MDC/MDIO pads are not muxed. `CpswMin_poll()` returns
link up unconditionally (there is nothing to read). A port with
`fixedSpeed == 0` makes `CpswMin_open()` fail: PHY-over-MDIO is not
implemented in this driver.

### Open sequence
1. `SOC_moduleClockEnable(CPTS)`, `SOC_moduleSetClockFrequency(CPTS, SYS_CLK,
   250 MHz)` and `(CPSW_5_50_250, DPLL_CORE_HSDIV0_CLKOUT1, 500 MHz)`: the
   values SysConfig generates in `ti_power_clock_config.c` for the SOEM CPSW
   example.
2. MSS_CTRL `CPSW_CONTROL.PORTn_MODE_SEL` = 0 (MII), after
   `SOC_controlModuleUnlockMMR(MAIN, MSS_CTRL_PARTITION0)`.
3. Reload safety (a JTAG reload does not reset the CPSW): per configured MAC
   port set `CMD_IDLE`, wait `MAC_STATUS.IDLE`, `MAC_SOFT_RESET`; then CPDMA
   `SOFT_RESET`. This is what stops a channel still pointing into the previous
   image (the LLD does the CPDMA reset first thing in `EnetCpdma_open` and the
   MAC idle+reset in `CpswMacPort_open`). All waits are bounded (10000 x `ClockP_usleep(1)`, at least 10 ms).
4. Host port and ALE as the AM243x driver: ALE enable+clear+bypass, host and
   MAC port states forward, `CPSW_CONTROL` = P0 enable + RX pad + TX CRC
   remove, RX max length, source MAC. ALE bypass sends everything received on
   MAC ports to the host; host TX is directed to a port with the descriptor
   `TO_PORT_EN`/`TO_PORT` bits (the AM243x `dstTag` equivalent).
5. TX/RX channel enable, `RX_BUFFER_OFFSET` = 0, RX queue loaded, and only
   then `MAC_CONTROL` = `GMII_EN | FULLDUPLEX` on the ports, so no frame can
   arrive before a descriptor exists.

### Descriptors
16-byte CPPI 3.0 descriptor (`next`, `bufPtr`, `bufOffLen`, `flags`) padded to
32 B. The 24 descriptors (768 B) are file-static in the NOLOAD output section
`.bss:ENET_CPPI_DESC`, placed by the project SysConfig into the 8 KB
`CPPI_DESC` region at 0x70040000, which the MPU maps NonCached (same scheme as
the SOEM/Enet LLD example; the LLD also rejects cached descriptors). The
hardware writes `flags` in place (OWNER cleared, EOQ set) while software
links `next`, so with cached descriptors a write-back of the line could
overwrite the hardware's update. Packet buffers stay in cached
OCRAM inside the driver object, with `CacheP_wb` before TX and `CacheP_inv`
before handing an RX buffer to the hardware and again on receive, as in the
AM243x driver. A `dsb` precedes every HDP or link write.

### Queues and EOQ restart
Each direction keeps a small ring of descriptor indices in hardware chain
order (`txQ`, `rxQ`; free-running counters), because buffers can be returned
out of order but the hardware walks the chain in append order.
* Append: point the current tail's `next` at the new descriptor; if the
  queue is empty, the channel is stopped, so write HDP directly.
* If the hardware had read the tail's `next` as 0 before the link, it stops
  after the tail and sets EOQ. That is only observable when the tail is
  reaped, so reaping (`txReap` for TX, `CpswMin_recv` for RX) restarts the
  channel by writing HDP with the reaped descriptor's `next` when `EOQ` is
  set and `next != 0`. This is the same rule as `EnetCpdma_dequeueTx/Rx`.
  HDP is never rewritten while a queue is non-empty and not EOQ-stopped.
* A stranded TX is restarted promptly because `CpswMin_recv()` calls
  `txReap()` whenever no RX frame is pending (the echo loop polls `recv`
  continuously), and `getTxBuf()`, `send()` and `poll()` reap too.
* RX completion = `OWNER` clear; length = `flags[11:0]`, ingress port =
  `flags[18:16]`; `flags & 0x03700000` (long, short, overrun, packet error)
  counts as `rxErrs` and the buffer is requeued. CP registers are not
  written (no interrupts are enabled, so there is nothing to acknowledge).

### Zero-copy echo (`CpswMin_sendRx`)
Same contract as the AM243x version. The caller edits a received buffer in
place and `CpswMin_sendRx(h, rx, len, port)` transmits it directly:
* A free TX descriptor is popped from `txFree`; `txRx[i] = rxIndex + 1`
  records the lend, and the TX descriptor's `bufPtr` is the RX buffer itself
  (`txQueue()`: `bufPtr`, `bufOffLen` and `flags` length set from `len`, SOP |
  EOP | OWNER | TO_PORT; cache write-back of the RX buffer; HDP write if the TX
  queue was empty, else link behind the tail). The descriptor's own TX buffer
  stays unused. Frames under 60 B are zero-padded in place.
* The RX buffer is not requeued by the caller. `txReap()` re-initialises the
  RX descriptor (`rxQueue()`: next = 0, `bufPtr`, `bufOffLen` = buffer size,
  OWNER, cache invalidate of the buffer) and re-links it at the RX tail, or
  writes `RX_HDP` if the RX chain had drained, only after OWNER cleared on the
  TX descriptor. The RX chain therefore shrinks while frames are in TX;
  16 RX descriptors (`CPSW_MIN_NUM_RX`) against 8 TX leave headroom.
* TX completions are reaped in `CpswMin_recv()` only when it has no RX frame
  to return, and in `sendRx` only when `txFreeCnt == 0`. TX EOQ restart works
  as above (`TX_HDP` = reaped `next`).
* On failure (bad index, length, no free TX descriptor) it returns
  `SystemP_FAILURE` and the caller keeps the buffer (`CpswMin_recvDone()`).
* Descriptor pool: 8 TX + 16 RX x 32 B = 768 B of the 8 KB `CPPI_DESC` region.

### Teardown
The driver has no close API (same as AM243x). Teardown is the reset at open
(MAC idle + soft reset, CPDMA `SOFT_RESET`, which clears HDP/CP). The
graceful per-channel handshake the LLD uses on close (`TX/RX_TEARDOWN`, wait
`CP == 0xFFFFFFFC`) is not needed because the next open resets the block.

## Register offsets verified against the CSL

All checked by `_Static_assert` in `cpsw_min_cpdma_csl_check.c`, by macro and
(where a structure member exists) by `offsetof`. Headers are under
`C:\ti\mcu_plus_sdk_am261x_11_00_00_29\source\networking\enet\hw_include`
(SoC selects `xge/V6`, `ale/V5`, `cpdma/V1`, `mdio/V4`) unless noted.

| Item | Offset (from block base) | Header |
|---|---|---|
| CPSW_NUSS base 0x52800000 | `CSL_CPSW0_U_BASE` | `drivers/hw_include/am261x/cslr_soc_baseaddress.h` |
| CPSW_NU (switch core) | NUSS + 0x20000 | `xge/V6/cslr_xge_cpsw_ss_s.h` `CPSW_NU_OFFSET` |
| CPDMA | NUSS + 0x34000 (`CPSW_CPDMA_OFFSET`) | same |
| ALE | NUSS + 0x3E000 (`CPSW_ALE_OFFSET`) | same |
| Ethernet port n (n = 0 is MAC port 1) | NU + 0x2000 + n * 0x1000 | `CPSW_ETHX_OFFSET(n)` |
| CPSW CONTROL_REG | NU + 0x004 | `xge/V6/cslr_xge_cpsw.h` |
| P0_RX_MAXLEN_REG | NU + 0x1024 | same |
| PN_RX_MAXLEN / SA_L / SA_H | port + 0x024 / 0x308 / 0x30C | same |
| PN_MAC_CONTROL / MAC_STATUS / MAC_SOFT_RESET | port + 0x330 / 0x334 / 0x338 | same |
| CONTROL bits P0_ENABLE (2), TX_CRC_REMOVE (13), RX_PAD (14) | | same |
| MAC_CONTROL bits FULLDUPLEX (0), GMII_EN (5), CMD_IDLE (11); MAC_STATUS IDLE (31) | | same |
| ALE CONTROL / PORTCTL[n] | ALE + 0x008 / 0x040 + 4n (3 port registers) | `ale/V5/cslr_ale.h` |
| ALE enable (31), clear (30), bypass (4), port state mask 3 | | same |
| CPDMA TX_CONTROL / RX_CONTROL | CPDMA + 0x004 / 0x014 | `cpdma/V1/cslr_cpdma.h` |
| CPDMA SOFT_RESET / DMASTATUS / RX_BUFFER_OFFSET | + 0x01C / 0x024 / 0x028 | same |
| CPDMA TX_HDP[0] / RX_HDP[0] | + 0x200 / 0x220 (by `offsetof(SRAM.*_HDP)`) | same |
| DMASTATUS host error codes | RX [15:12], TX [23:20] | same |
| MSS_CTRL `CPSW_CONTROL` | MSS_CTRL (0x50D00000) + 0x860 | `drivers/hw_include/am261x/cslr_mss_ctrl.h` |
| PORT1_MODE_SEL [2:0], PORT2_MODE_SEL [18:16], 0 = MII | | same |

Offsets reused from the AM243x `cpsw_min.c` and now confirmed for the AM261x:
CPSW CONTROL, P0_RX_MAXLEN, the per-port block, ALE CONTROL/PORTCTL. Not
carried over because they are PKTDMA/AM243x only: `P0_FLOW_ID_OFFSET`,
`ALE_THREADMAPDEF`, the ring/channel real-time registers, Sciclient, and the
CTRL_MMR0 `ENET_CTRL` interface select (replaced by MSS_CTRL `CPSW_CONTROL`).

Checked by reading `core/src/dma/cpdma/enet_cpdma_priv.h` and `enet_cpdma.c`
(not machine-checked, these are in the LLD source, not the CSL):
descriptor layout and flag bits (SOP 31, EOP 30, OWNER 29, EOQ 28, TO_PORT_EN
20, TO_PORT [17:16], FROM_PORT [18:16], RX error bits 25/24/22/21:20), the
need for uncached descriptors (`EnetCpdma_open` rejects cached ones), the
EOQ restart rule, `TO_PORT = macPort` for directed TX, and the MII
`MAC_CONTROL` = `GMII_EN | FULLDUPLEX` (`CpswMacPort_enablePort`, MII case).

## IO expander and pinmux

What the SOEM example (`C:\t\ecat-echo\examples\soem_slaveinfo\am261x-lp-cpsw`)
does: `board_cpsw.c` `ecnic_enet_board_init()` does **not** drive anything.
It reads U42 for the log and *releases to input* U42 (I2C 0x21) P3
(MDIO/MDC_MUX_SEL1, Eth Conn 0) and U23 (I2C 0x20) P4 (BP_BO_MUX_EN, enable
of the CPSW MDIO level translator), returning them to the board defaults
(select high = ICSS PR0_MDIO0, translator off). `enet_board_fixed_link.c`
deliberately skips `EnetBoard_setMacPort2IOExpanderCfg`. The stock Enet LLD
flow (U42 P3 low, U23 P4 high) is for CPSW MDIO and is **not** reproduced.

`boardInit()` in `cpsw_min_example.c` does the same release for the same two
pins through the SDK `TCA6408_*` driver (I2C0, GPIO134/135, instance
CONFIG_I2C0 from the project SysConfig), then reads U42 input register 0 and
logs `U42 pins 0x.. (CONN0_DET, RST0, SEL1, SEL2)`. Eth Conn 1's MDIO select
(U42 P4, SEL2) is not touched; it should read 1 (ICSS). The MII data and
clock pads need no routing.

Pinmux (`gCpswPins`, values from the SOEM `ti_pinmux_config.c`, all
`PIN_MODE(4) | PIN_PULL_DISABLE | PIN_SLEW_RATE_LOW`): MII2 RXD0..3 =
GPIO93..96, RXCLK GPIO91, RXDV GPIO92, RX_ER GPIO87, TXD0..3 = GPIO99..102,
TXCLK GPIO97, TX_EN GPIO98, COL GPIO88, CRS GPIO89. MDIO0 (GPIO41/42) is not
configured. MII1 pads are not configured (port 1 unused). UART0 (lean, in
`min_log.c`): GPIO27 RXD / GPIO28 TXD mode 0, LIN0_UART0 clock 160 MHz from
DPLL_PER_HSDIV0_CLKOUT2, 115200 8N1.

## Build

Environment-specific overrides are needed because the SDK `imports.mak`
points at ccs2030 / sysconfig_1.25 / ti-cgt-armllvm 4.0.3, which are not
installed here (ccs2100, sysconfig_1.26.0, ti-cgt-armllvm 4.0.4 are), and
because the repo is not checked out under `<sdk>/source/networking/enet/core`
(the default `MCU_PLUS_SDK_PATH` is derived from that depth, so set it).
PowerShell:

```
$env:MCU_PLUS_SDK_PATH = 'C:/ti/mcu_plus_sdk_am261x_11_00_00_29'
cd C:\t\enet-min-am261\examples\enet_min\cpsw\am261x-lp\r5fss0-0_nortos\ti-arm-clang
C:\ti\ccs2100\ccs\utils\bin\gmake.exe cpsw_min_example.release.out `
  CCS_PATH=C:/ti/ccs2100/ccs CGT_TI_ARM_CLANG_PATH=C:/ti/ti-cgt-armllvm_4.0.4.LTS `
  SYSCFG_PATH=C:/ti/sysconfig_1.26.0
# lean: cd ...\cpsw_lean\am261x-lp\r5fss0-0_nortos\ti-arm-clang, target cpsw_min_lean.release.out
```

Results: both link with exit 0 and no compiler warnings (`-Wall -Werror`);
the only tool output besides compile lines is SysConfig `info:` notices
(shared-memory MPU note, UART baud). The boot-image (`.mcelf`) step was not
run. The build-time CSL check passes, so every offset in the table above
matches the CSL.

AM243x regression, built with `C:\ti\mcu_plus_sdk_am243x_12_01_00_33`
(`SYSCFG_PATH=C:/ti/sysconfig_1.27.0`, same CCS and compiler): `cpsw` and
`cpsw_lean` on `am243x-evm` and `am243x-lp`, `icssg_lean` and `hello_lean`
on `am243x-evm` (the last two use `min_log.c`). All link, exit 0, only the
SysConfig part-number notice.

## Sizes (from the `.map` files)

| | code + rodata | data | bss | notes |
|---|---|---|---|---|
| `cpsw_min_cpdma.obj` | 1,436 B | 0 | 0 | plus 512 B in `.bss:ENET_CPPI_DESC` |
| `cpsw_min_example.obj` | 1,091 B | 0 | 24,704 B | `gCpsw`: 24,576 B of buffers |
| `min_log.obj` (lean) | 1,061 B | 0 | 1 B | |
| `cpsw_min.obj` (AM243x, for comparison) | 2,858 B | 0 | 0 | PKTDMA + Sciclient + MDIO/PHY |
| `cpsw_min_example.release.out`, whole image | 33,683 B | 810 B | 25,832 B | UART driver, printf, I2C driver |
| `cpsw_min_lean.release.out`, whole image | 15,547 B | 164 B | 24,941 B | polled UART, I2C driver |
| SOEM `enet-cpsw` lib (Enet LLD, for comparison) | 70,581 B | 2,340 B | 2,708 B | `soem_slaveinfo.release.map` |

The driver is 1.4 KB against about 69 KB for the Enet LLD (the SOEM lib total
is 70,581 B), although the LLD figure includes MDIO/PHY, CPTS, stats, EST,
IOCTLs and the RTOS glue that the fixed-link port does not have.

## `.out` files to load

* `C:\t\enet-min-am261\examples\enet_min\cpsw\am261x-lp\r5fss0-0_nortos\ti-arm-clang\cpsw_min_example.release.out` (UART log through the SDK UART driver)
* `C:\t\enet-min-am261\examples\enet_min\cpsw_lean\am261x-lp\r5fss0-0_nortos\ti-arm-clang\cpsw_min_lean.release.out` (polled UART0, 115200 8N1)

Both run on R5FSS0-0 (core 0), nortos, entry `_vectors`; code and data load
from 0x70042000, descriptors at 0x70040000.

## What I am unsure about, and what to check first

1. **EOQ / HDP behaviour.** The restart logic assumes the CPDMA stops after a
   descriptor whose `next` it read as 0, sets EOQ, and accepts a fresh HDP
   write afterwards (the LLD relies on the same). If the first RX or TX
   stalls after a few frames, look at `CPDMA_RX_HDP` / `TX_HDP` and the EOQ
   bit of the stalled descriptor.
2. **Descriptor region.** Check in the linker map that `gTxDesc/gRxDesc` are
   at 0x70040000 (they are in the built maps) and that the NonCached MPU
   region is the one in `ti_dpl_config.c` (region 4, 8 KB, shareable
   non-cacheable). If descriptors ended up cached the symptom is OWNER never
   clearing.
3. **First log lines to expect** (standard `.out`): `U42 pins 0x..` (SEL1 and
   SEL2 should read 1 = ICSS; note CONN0_DET), `open done`, then `port 2 link up, speed code
   1` (that is unconditional), then `tx/rx/rxErr/dmaErr` every 5 s. `dmaErr`
   must stay 0x0; nonzero means a CPDMA host error (bad descriptor address
   or ownership).
4. **Frames in/out.** With the SOEM `x` tester or any raw-frame sender: `rx`
   increasing means the MII pads, MAC enable, ALE bypass and RX DMA work;
   `tx` increasing with the tester seeing the 1 s broadcast (EtherType
   0x88B5) means directed TX works. If `rx` stays 0, check the PHY card is
   linked (its own autonegotiation, no MDIO here), then `PN_MAC_STATUS`.
5. **Unverified assumptions**: TX/RX channel mapping of untagged frames
   to CPDMA channel 0 (tagged frames with nonzero priority may map to other
   channels and be dropped; `P0_RX_PRI_MAP` is left at reset); TX
   per-priority max length registers are left at reset (as AM243x); the
   expander release needs working I2C0 (if `U42 read failed` appears, the
   rest still runs); `min_log` on AM261x UART0 (pins, 160 MHz clock, 16x
   divisor) is untested; clock writes in `CpswMin_open` copy the SOEM
   SysConfig output but have not been run; MSS_CTRL partition 0 is unlocked
   (not re-locked), as the LLD does.
6. A JTAG reload leaves the previous image's CPDMA RX running until
   `CpswMin_open()` resets it; frames arriving in that window can write into
   OCRAM. Prefer a full reset before load if the first run after a reload is
   flaky.
