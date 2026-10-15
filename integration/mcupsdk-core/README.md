# mcupsdk-core integration

The Enet LLD is built from the MCU+ SDK (`mcupsdk-core`), which owns the
library makefiles and the list of examples. The patches apply in order to
`mcupsdk-core` (tested on the 12.01 release commit).

`0001-*.patch` adds:

- the `enet-cpsw-icssg` library for AM243x R5F (`project_am243x_cpsw_icssg.js`,
  `project_cpsw_icssg.js` and the generated `makefile.cpsw_icssg.*`);
- registration of the library and the `enet_layer2_icssg_cpsw` example in
  `.project/device/project_am243x.js`, with the regenerated top-level
  makefiles and TIREX metadata.

`0002-*.patch` adds:

- the `lwipif-cpsw-icssg-freertos` library, the lwIP interface built for both
  peripherals (`project_cpsw_icssg_lwipif_freertos.js` and the generated
  `makefile.lwipif-cpsw-icssg-freertos.*`);
- registration of the library and the `enet_lwip_icssg_cpsw` example, with
  the regenerated top-level makefiles and TIREX metadata.

Apply them from the root of `mcupsdk-core`:

```
git am <path-to-enet-lld>/integration/mcupsdk-core/000*.patch
make -s -f makefile.am243x enet-cpsw-icssg_r5f.ti-arm-clang.freertos
make -s -C source/networking/enet/core/examples/enet_layer2_icssg_cpsw/am243x-evm/r5fss0-0_freertos/ti-arm-clang
make -s -f makefile.am243x lwipif-cpsw-icssg-freertos_r5f.ti-arm-clang
make -s -C source/networking/enet/core/examples/lwip/enet_lwip_icssg_cpsw/am243x-evm/r5fss0-0_freertos/ti-arm-clang
```
