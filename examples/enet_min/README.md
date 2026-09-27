# enet_min: minimal bare-metal CPSW and ICSSG Ethernet drivers (AM243x)

Two self-contained drivers, one `.c` + one `.h` each, plus an echo example
for each. No Enet LLD, no UDMA LLD, no PRUICSS driver, no RTOS and no
SysConfig Ethernet module. The only SDK pieces used are the ones a
nortos `hello_world` already links: Sciclient (resource and power
management through the TISCI firmware), CacheP/ClockP, Pinmux and DebugP.

| File | What it is |
|---|---|
| `cpsw_min.c/.h` | CPSW3G: ALE bypass, host port, MAC ports 1/2, DP83867 PHY, MDIO manual mode |
| `icssg_min.c/.h` | ICSSG dual-MAC firmware load/config, one slice per object, DP83869 PHY |
| `cpsw_min_example.c` | CPSW port 1 echo + 1 s broadcast test frame |
| `icssg_min_example.c` | ICSSG1 port 1 echo + 1 s broadcast test frame |
| `{cpsw,icssg}/am243x-evm/r5fss0-0_nortos` | Projects (hello_world SysConfig: UART log, MPU, clocks only) |

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

## Build

```
gmake -C cpsw/am243x-evm/r5fss0-0_nortos/ti-arm-clang all
gmake -C icssg/am243x-evm/r5fss0-0_nortos/ti-arm-clang all
```

The makefiles expect this repo at `source/networking/enet/core` inside the
MCU+ SDK, like the other Enet examples.

## Status

Not yet tested on hardware. Open items: the ICSSG core clock is left at the
boot default (set `ICSSG_MIN_CORE_CLK_HZ` to force one), and the VBUSM QoS
priority settings done by the Enet LLD are skipped.
