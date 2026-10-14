/*
 *  Copyright (C) 2026 Texas Instruments Incorporated
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *    Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *    Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * cpsw_min_example: CPSW MAC port 1 (DP83867 on the AM243x EVM, DP83869 on
 * the RGMII1 jack of the AM243x LP) with the cpsw_min driver. On the LP-AM261x
 * (ENET_MIN_BOARD_AM261X_LP) it is MAC port 2, a fixed 100 Mb/s full-duplex
 * MII link to the DP83826E card on Eth Conn 0. Every received frame is sent
 * back to its sender, and a broadcast test frame goes out once a second
 * while the link is up. Counters are printed every 5 seconds.
 */

#include <string.h>
#include <kernel/dpl/ClockP.h>
#include <kernel/dpl/DebugP.h>
#include <kernel/dpl/SystemP.h>
#include <drivers/pinmux.h>
#if defined(ENET_MIN_BOARD_AM243X_LP)
#include <drivers/gpio.h>
#include <drivers/hw_include/cslr_soc.h>
#endif
#if defined(ENET_MIN_BOARD_AM261X_LP)
#include <drivers/i2c.h>
#include <board/ioexp/ioexp_tca6408.h>
#include "ti_drivers_config.h"
#include "ti_drivers_open_close.h"
#endif
#include "cpsw_min.h"

#define TEST_ETHERTYPE      (0x88B5U)   /* IEEE local experimental */

/* RGMII1 + MDIO (values from the Enet SysConfig output). The RGMII1 pins
 * are the same on both boards; the LP routes MDIO0 to the PRG1_MDIO0 pins,
 * which it shares with PRU_ICSSG1, and has a PHY reset and an RGMII1 mux
 * select on GPIO0_26/27 (GPMC0_AD11/12). */
#define RGMII_IN            (PIN_MODE(4) | PIN_INPUT_ENABLE | PIN_PULL_DISABLE)
#if defined(ENET_MIN_BOARD_AM261X_LP)
#define PHY_ADDR            (0U)        /* unused: fixed link, MDIO untouched */
#define MAC_PORT            (2U)
#elif defined(ENET_MIN_BOARD_AM243X_LP)
#define PHY_ADDR            (3U)
#if defined(CPSW_MIN_LP_REV_E2)
/* E1/E2 boards: the mux select is GPIO0_70 (ball AA11), as in the SDK's
 * generated LP board config; GPIO0_26/27 drive the user LEDs there. */
#define GPIO_MUX_SEL        (70U)
#else
#define GPIO_PHY_RST        (26U)
#define GPIO_MUX_SEL        (27U)
#endif
#ifndef CPSW_MIN_LP_MUX_LEVEL
#define CPSW_MIN_LP_MUX_LEVEL   (1)
#endif
#else
#define PHY_ADDR            (0U)
#endif
#ifndef MAC_PORT
#define MAC_PORT            (1U)
#endif
static const Pinmux_PerCfg_t gCpswPins[] =
{
#if defined(ENET_MIN_BOARD_AM261X_LP)
    /* MII2 on the GPIO91.. pads (values from the Enet SysConfig output);
     * the CPSW MDIO pads GPIO41/42 are left alone */
#define MII_PIN             (PIN_MODE(4) | PIN_PULL_DISABLE | PIN_SLEW_RATE_LOW)
    { PIN_GPIO93,  MII_PIN },                                    /* RXD0  */
    { PIN_GPIO94,  MII_PIN },                                    /* RXD1  */
    { PIN_GPIO95,  MII_PIN },                                    /* RXD2  */
    { PIN_GPIO96,  MII_PIN },                                    /* RXD3  */
    { PIN_GPIO91,  MII_PIN },                                    /* RXCLK */
    { PIN_GPIO92,  MII_PIN },                                    /* RXDV  */
    { PIN_GPIO87,  MII_PIN },                                    /* RX_ER */
    { PIN_GPIO99,  MII_PIN },                                    /* TXD0  */
    { PIN_GPIO100, MII_PIN },                                    /* TXD1  */
    { PIN_GPIO101, MII_PIN },                                    /* TXD2  */
    { PIN_GPIO102, MII_PIN },                                    /* TXD3  */
    { PIN_GPIO97,  MII_PIN },                                    /* TXCLK */
    { PIN_GPIO98,  MII_PIN },                                    /* TX_EN */
    { PIN_GPIO88,  MII_PIN },                                    /* COL   */
    { PIN_GPIO89,  MII_PIN },                                    /* CRS   */
#elif defined(ENET_MIN_BOARD_AM243X_LP)
#if defined(CPSW_MIN_LP_REV_E2)
    { PIN_PRG1_PRU1_GPO5,  PIN_MODE(7) | PIN_INPUT_ENABLE | PIN_PULL_DISABLE },    /* MUX SEL */
#else
    { PIN_GPMC0_AD11,      PIN_MODE(7) | PIN_INPUT_ENABLE | PIN_PULL_DIRECTION },  /* PHY RST */
    { PIN_GPMC0_AD12,      PIN_MODE(7) | PIN_INPUT_ENABLE | PIN_PULL_DIRECTION },  /* MUX SEL */
#endif
    { PIN_PRG1_MDIO0_MDC,  PIN_MODE(4) | PIN_PULL_DISABLE },     /* MDC   */
    { PIN_PRG1_MDIO0_MDIO, RGMII_IN },                           /* MDIO  */
#else
    { PIN_PRG0_PRU1_GPO19, PIN_MODE(4) | PIN_PULL_DISABLE },     /* MDC   */
    { PIN_PRG0_PRU1_GPO18, RGMII_IN },                           /* MDIO  */
#endif
#if !defined(ENET_MIN_BOARD_AM261X_LP)
    { PIN_PRG0_PRU1_GPO7,  RGMII_IN },                           /* RD0   */
    { PIN_PRG0_PRU1_GPO9,  RGMII_IN },                           /* RD1   */
    { PIN_PRG0_PRU1_GPO10, RGMII_IN },                           /* RD2   */
    { PIN_PRG0_PRU1_GPO17, RGMII_IN },                           /* RD3   */
    { PIN_PRG0_PRU0_GPO10, RGMII_IN },                           /* RXC   */
    { PIN_PRG0_PRU0_GPO9,  RGMII_IN },                           /* RX_CTL*/
    { PIN_PRG1_PRU1_GPO7,  RGMII_IN },                           /* TD0   */
    { PIN_PRG1_PRU1_GPO9,  RGMII_IN },                           /* TD1   */
    { PIN_PRG1_PRU1_GPO10, RGMII_IN },                           /* TD2   */
    { PIN_PRG1_PRU1_GPO17, RGMII_IN },                           /* TD3   */
    { PIN_PRG1_PRU0_GPO10, RGMII_IN },                           /* TXC   */
    { PIN_PRG1_PRU0_GPO9,  RGMII_IN },                           /* TX_CTL*/
#endif
    { PINMUX_END, 0U }
};

static CpswMin_Obj gCpsw;

static const CpswMin_Cfg gCpswCfg =
{
    .macAddr  = { 0x02U, 0x00U, 0x00U, 0x00U, 0x43U, 0x01U },
    .numPorts = 1U,
#if defined(ENET_MIN_BOARD_AM261X_LP)
    .port     = { { .macPort = MAC_PORT, .phyAddr = PHY_ADDR,
                    .fixedSpeed = CPSW_MIN_FIXED(CPSW_MIN_SPEED_100) } },
#else
    .port     = { { .macPort = MAC_PORT, .phyAddr = PHY_ADDR } },
#endif
};

#if defined(ENET_MIN_BOARD_AM261X_LP)

/*
 * LP-AM261 RevE2 IO expanders (TCA6408, from the PROC193E2 schematic):
 *   U42 (0x21) P3 MDIO/MDC_MUX_SEL1: Eth Conn 0 MDIO, high (default, pull-up)
 *       = ICSS PR0_MDIO0, low = CPSW MDIO0
 *   U23 (0x20) P4 BP_BO_MUX_EN: output enable of the CPSW MDIO level
 *       translator, off by default (R90 pulls it low)
 * The MII data pads need no routing and the CPSW MDC pad is not level-shifted,
 * so the link runs with MDIO untouched. The expander outputs survive SoC and
 * JTAG resets, so give both pins back to their board defaults (inputs) in
 * case an earlier program drove them, and log U42.
 */
#define U42_I2C_ADDR        (0x21U)
#define U42_MDIO_MUX_SEL1   (3U)
#define U23_I2C_ADDR        (0x20U)
#define U23_BP_BO_MUX_EN    (4U)

static void expanderRelease(uint32_t addr, uint32_t pin)
{
    TCA6408_Config cfg;
    TCA6408_Params params;

    TCA6408_Params_init(&params);
    params.i2cAddress = addr;
    if (TCA6408_open(&cfg, &params) == SystemP_SUCCESS)
    {
        (void)TCA6408_config(&cfg, pin, TCA6408_MODE_INPUT);
        TCA6408_close(&cfg);
    }
}

static void boardInit(void)
{
    I2C_Transaction t;
    uint8_t reg = 0U, in = 0U;

    expanderRelease(U42_I2C_ADDR, U42_MDIO_MUX_SEL1);
    expanderRelease(U23_I2C_ADDR, U23_BP_BO_MUX_EN);

    I2C_Transaction_init(&t);
    t.targetAddress = U42_I2C_ADDR;
    t.writeBuf      = &reg;
    t.writeCount    = 1U;
    t.readBuf       = &in;
    t.readCount     = 1U;
    if (I2C_transfer(gI2cHandle[CONFIG_I2C0], &t) == SystemP_SUCCESS)
    {
        DebugP_log("cpsw_min: U42 pins 0x%02x (CONN0_DET %u, RST0 %u, SEL1 %u)\r\n",
                   in, in & 1U, (in >> 1) & 1U, (in >> 3) & 1U);
    }
    else
    {
        DebugP_log("cpsw_min: U42 read failed\r\n");
    }
}

#endif

static void sendTest(CpswMin_Obj *h, uint32_t seq)
{
    uint8_t *buf = CpswMin_getTxBuf(h);

    if (buf != NULL)
    {
        memset(buf, 0xFF, 6U);
        memcpy(&buf[6], gCpswCfg.macAddr, 6U);
        buf[12] = (uint8_t)(TEST_ETHERTYPE >> 8);
        buf[13] = (uint8_t)TEST_ETHERTYPE;
        memcpy(&buf[14], &seq, sizeof(seq));
        (void)CpswMin_send(h, buf, 64U, MAC_PORT);
    }
}

static void echo(CpswMin_Obj *h, const uint8_t *rx, uint32_t len, uint32_t port)
{
    uint8_t *buf = CpswMin_getTxBuf(h);

    if (buf != NULL)
    {
        memcpy(buf, &rx[6], 6U);
        memcpy(&buf[6], gCpswCfg.macAddr, 6U);
        memcpy(&buf[12], &rx[12], len - 12U);
        (void)CpswMin_send(h, buf, len, port);
    }
}

void cpsw_min_example_main(void *args)
{
    uint64_t now, nextPoll = 0U, nextTest = 0U, nextLog = 0U;
    uint32_t link = 0U, seq = 0U, len, port;
    uint8_t *rx;

    Pinmux_config(gCpswPins, PINMUX_DOMAIN_ID_MAIN);
#if defined(ENET_MIN_BOARD_AM261X_LP)
    boardInit();
#elif defined(ENET_MIN_BOARD_AM243X_LP)
    /* PHY out of reset, RGMII1 jack to CPSW (as the Enet LP examples do) */
#if defined(GPIO_PHY_RST)
    GPIO_setDirMode(CSL_GPIO0_BASE, GPIO_PHY_RST, GPIO_DIRECTION_OUTPUT);
    GPIO_pinWriteHigh(CSL_GPIO0_BASE, GPIO_PHY_RST);
#endif
    GPIO_setDirMode(CSL_GPIO0_BASE, GPIO_MUX_SEL, GPIO_DIRECTION_OUTPUT);
#if (CPSW_MIN_LP_MUX_LEVEL != 0)
    GPIO_pinWriteHigh(CSL_GPIO0_BASE, GPIO_MUX_SEL);
#else
    GPIO_pinWriteLow(CSL_GPIO0_BASE, GPIO_MUX_SEL);
#endif
#endif

    if (CpswMin_open(&gCpsw, &gCpswCfg) != SystemP_SUCCESS)
    {
        DebugP_log("cpsw_min: open failed\r\n");
        return;
    }
    DebugP_log("cpsw_min: open done, waiting for link\r\n");

    for (;;)
    {
        now = ClockP_getTimeUsec();

        while ((rx = CpswMin_recv(&gCpsw, &len, &port)) != NULL)
        {
            if (len >= 14U)
            {
                echo(&gCpsw, rx, len, port);
            }
            CpswMin_recvDone(&gCpsw, rx);
        }

        if (now >= nextPoll)
        {
            uint32_t newLink = CpswMin_poll(&gCpsw);
            if (newLink != link)
            {
                DebugP_log("cpsw_min: port %u link %s, speed code %u\r\n", MAC_PORT,
                           (newLink != 0U) ? "up" : "down", gCpsw.port[0].speed);
                link = newLink;
            }
            nextPoll = now + 100000U;
        }

        if ((link != 0U) && (now >= nextTest))
        {
            sendTest(&gCpsw, seq++);
            nextTest = now + 1000000U;
        }

        if (now >= nextLog)
        {
#if defined(SOC_AM261X)
            DebugP_log("cpsw_min: tx %u rx %u rxErr %u dmaErr 0x%x\r\n",
                       gCpsw.txPkts, gCpsw.rxPkts, gCpsw.rxErrs, gCpsw.dmaErr);
#else
            DebugP_log("cpsw_min: tx %u rx %u rxErr %u\r\n",
                       gCpsw.txPkts, gCpsw.rxPkts, gCpsw.rxErrs);
#endif
            nextLog = now + 5000000U;
        }
    }
}
