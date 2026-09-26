# Enet lwIP ICSSG + CPSW (AM243x EVM)

This example runs lwIP on one R5F core (R5FSS0-0, FreeRTOS) with three
Ethernet ports, each with its own netif:

| Netif            | Enet instance       | Peripheral                   |
|------------------|---------------------|------------------------------|
| `NETIF_INST_ID0` | `CONFIG_ENET_ICSS0` | ICSSG1, dual-MAC, MAC port 1 |
| `NETIF_INST_ID1` | `CONFIG_ENET_ICSS1` | ICSSG1, dual-MAC, MAC port 2 |
| `NETIF_INST_ID2` | `CONFIG_ENET_CPSW0` | CPSW3G, MAC port 1           |

Every netif gets its address from DHCP. Once one netif is up, a TCP echo
server listens on port 8888 of all netifs.

## lwIP in combo mode

The SysConfig file adds both `enet_icss` and `enet_cpsw`, so the Enet
SysConfig scripts generate the combined (combo) configuration described in
`examples/enet_layer2_icssg_cpsw/README.md`. For lwIP this means:

- Netif indices are global, ICSSG netifs first, then CPSW netifs. The
  `NETIF_INST_IDx` names and `ENET_SYSCFG_NETIF_COUNT` from `ti_enet_config.h`
  cover both peripherals.
- `LwipifEnetApp_netifOpen()`, `LwipifEnetApp_startSchedule()` and the rest of
  the `ti_enet_lwipif.h` API take the global index and route it to the
  peripheral that owns the netif.
- The callbacks of the Enet lwIP interface library are dispatched on the Enet
  type; DMA channel ids are global like the netif indices.
- The RX custom pbuf pools of both peripherals are set up when the first netif
  opens, and both share one free pbuf node array.
- Only one netif of both modules may be the default netif.

The lwIP interface library takes the MTU and the RX polling period from the
peripheral of the first netif opened; keep them equal on both modules (the
SysConfig defaults are).

## Build

Link `enet-cpsw-icssg.am243x.r5f.<cgt>.freertos.<profile>.lib` and
`lwipif-cpsw-icssg-freertos.am243x.r5f.<cgt>.<profile>.lib` (both built with
`ENET_ENABLE_PER_CPSW` and `ENET_ENABLE_PER_ICSSG`), and build the application
with both defines. The mcupsdk-core patches in `integration/mcupsdk-core` add
the libraries and this example to the SDK build.
