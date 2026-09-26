# Enet Layer 2 ICSSG + CPSW (AM243x EVM)

This example opens three Ethernet ports from one R5F core (R5FSS0-0, FreeRTOS):

| Enet instance       | Peripheral                  | DMA channels           |
|---------------------|-----------------------------|------------------------|
| `CONFIG_ENET_ICSS0` | ICSSG1, dual-MAC, MAC port 1 | `ENET_DMA_TX_CH0` / `ENET_DMA_RX_CH0` |
| `CONFIG_ENET_ICSS1` | ICSSG1, dual-MAC, MAC port 2 | `ENET_DMA_TX_CH1` / `ENET_DMA_RX_CH1` |
| `CONFIG_ENET_CPSW0` | CPSW3G, MAC port 1           | `ENET_DMA_TX_CH_CPSW` / `ENET_DMA_RX_CH_CPSW` |

Each port echoes every received frame back to its sender. The UART menu
prints per-port statistics (`s`), resets them (`r`) or exits (`x`).

## How the combined configuration works

The SysConfig file adds both the `enet_icss` and the `enet_cpsw` modules.
When both are present, the Enet SysConfig scripts switch to *combo mode*
(`sysconfig/networking/.meta/common/enet_combo.syscfg.js`):

- Each module's generated code is renamed with an `_Icssg` / `_Cpsw` suffix.
- A generated dispatcher provides the public callbacks the Enet LLD and the
  application call (`EnetSoc_*`, `EnetApp_*`, `EnetBoard_*`, ...), routing
  on the Enet type, the instance index or the DMA channel.
- Enet instance and DMA channel indices are global: ICSSG first, then CPSW.
- Packet memory and the packet pool are shared and sized for both.

Combo mode currently has these limits:

- lwIP netifs are not supported (set the netif count to 0 on both modules).
- A custom board configuration is not supported.
- Packet pool, packet-info-only, MCM and RTOS variant settings must match
  between the ICSSG and CPSW instances.

The application must link `enet-cpsw-icssg.am243x.r5f.<cgt>.freertos.<profile>.lib`
and build with both `ENET_ENABLE_PER_CPSW` and `ENET_ENABLE_PER_ICSSG` defined.
