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
 * the RGMII1 jack of the AM243x LP) with the cpsw_min driver. Every
 * received frame is sent back to its sender, and a broadcast test frame
 * goes out once a second while the link is up. Counters are printed every
 * 5 seconds.
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
#include "cpsw_min.h"
#if defined(CPSW_MIN_TIMING)
#include "cpsw_min_timing.h"
#endif

#define TEST_ETHERTYPE      (0x88B5U)   /* IEEE local experimental */
#ifndef CPSW_MIN_POLL_US
#define CPSW_MIN_POLL_US    (100000U)   /* link poll period */
#endif

/* RGMII1 + MDIO (values from the Enet SysConfig output). The RGMII1 pins
 * are the same on both boards; the LP routes MDIO0 to the PRG1_MDIO0 pins,
 * which it shares with PRU_ICSSG1, and has a PHY reset and an RGMII1 mux
 * select on GPIO0_26/27 (GPMC0_AD11/12). */
#define RGMII_IN            (PIN_MODE(4) | PIN_INPUT_ENABLE | PIN_PULL_DISABLE)
#if defined(ENET_MIN_BOARD_AM243X_LP)
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
static const Pinmux_PerCfg_t gCpswPins[] =
{
#if defined(ENET_MIN_BOARD_AM243X_LP)
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
    { PINMUX_END, 0U }
};

static CpswMin_Obj gCpsw;

static const CpswMin_Cfg gCpswCfg =
{
    .macAddr  = { 0x02U, 0x00U, 0x00U, 0x00U, 0x43U, 0x01U },
    .numPorts = 1U,
    .port     = { { .macPort = 1U, .phyAddr = PHY_ADDR } },
};

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
        (void)CpswMin_send(h, buf, 64U, 1U);
    }
}

#if defined(CPSW_MIN_ECHO_COPY)
/* Copying echo: the reply is built in a TX buffer. */
static void echo(CpswMin_Obj *h, uint8_t *rx, uint32_t len, uint32_t port)
{
    uint8_t *buf = CpswMin_getTxBuf(h);

    if (buf != NULL)
    {
        memcpy(buf, &rx[6], 6U);
        memcpy(&buf[6], gCpswCfg.macAddr, 6U);
        memcpy(&buf[12], &rx[12], len - 12U);
        (void)CpswMin_send(h, buf, len, port);
    }
    CpswMin_recvDone(h, rx);
}
#else
/* Zero-copy echo: swap the MAC addresses in place and send the RX buffer
 * itself; the driver hands it back to RX once the frame is out. */
static void echo(CpswMin_Obj *h, uint8_t *rx, uint32_t len, uint32_t port)
{
    memcpy(rx, &rx[6], 6U);
    memcpy(&rx[6], gCpswCfg.macAddr, 6U);
    if (CpswMin_sendRx(h, rx, len, port) != SystemP_SUCCESS)
    {
        CpswMin_recvDone(h, rx);
    }
}
#endif

void cpsw_min_example_main(void *args)
{
    uint64_t now, nextPoll = 0U, nextTest = 0U, nextLog = 0U, lastRx = 0U;
    uint32_t lastRxPkts = 0U;
    uint32_t link = 0U, seq = 0U, len, port;
    uint8_t *rx;
#if defined(CPSW_MIN_TIMING)
    uint32_t tc0, tcIdle = 0U, tbatch = 0U;
#endif

    Pinmux_config(gCpswPins, PINMUX_DOMAIN_ID_MAIN);
#if defined(ENET_MIN_BOARD_AM243X_LP)
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
#if defined(CPSW_MIN_TIMING)
    tmInit(1U);
#endif

    for (;;)
    {
        now = ClockP_getTimeUsec();

#if defined(CPSW_MIN_TIMING)
        tmDrain();
        tc0 = CycleCounterP_getCount32();
        tmDropCheck(tc0, tbatch);
        tbatch = 0U;
        rx = CpswMin_recv(&gCpsw, &len, &port);
        if (rx == NULL)
        {
            if (tcIdle != 0U)
            {
                tmIdle(tc0 - tcIdle);
            }
            tcIdle = tc0;
        }
        while (rx != NULL)
        {
            /* echo() inlined so copy and send are timed apart */
            uint32_t tc1 = CycleCounterP_getCount32(), tc2 = tc1, tc3 = tc1, tc4;
            uint32_t tseq = tmSeq(rx);
#if defined(CPSW_MIN_ECHO_COPY)
            uint8_t *buf = CpswMin_getTxBuf(&gCpsw);

            if ((len >= 14U) && (buf != NULL))
            {
                memcpy(buf, &rx[6], 6U);
                memcpy(&buf[6], gCpswCfg.macAddr, 6U);
                memcpy(&buf[12], &rx[12], len - 12U);
                tc2 = CycleCounterP_getCount32();
                (void)CpswMin_send(&gCpsw, buf, len, port);
                tc3 = CycleCounterP_getCount32();
            }
            CpswMin_recvDone(&gCpsw, rx);
#else
            if (len >= 14U)
            {
                memcpy(rx, &rx[6], 6U);
                memcpy(&rx[6], gCpswCfg.macAddr, 6U);
                tc2 = CycleCounterP_getCount32();
                if (CpswMin_sendRx(&gCpsw, rx, len, port) != SystemP_SUCCESS)
                {
                    CpswMin_recvDone(&gCpsw, rx);
                }
                tc3 = CycleCounterP_getCount32();
            }
            else
            {
                CpswMin_recvDone(&gCpsw, rx);
            }
#endif
            tc4 = CycleCounterP_getCount32();
            tmSoftware(tseq, len, tc1 - tc0, tc2 - tc1, tc3 - tc2, tc4 - tc3);
            tcIdle = 0U;
            tbatch++;
            tc0 = CycleCounterP_getCount32();
            rx = CpswMin_recv(&gCpsw, &len, &port);
        }
#else
        while ((rx = CpswMin_recv(&gCpsw, &len, &port)) != NULL)
        {
            if (len >= 14U)
            {
                echo(&gCpsw, rx, len, port);    /* returns rx to the driver */
            }
            else
            {
                CpswMin_recvDone(&gCpsw, rx);
            }
        }
#endif

        if (now >= nextPoll)
        {
            uint32_t newLink = CpswMin_poll(&gCpsw);
            if (newLink != link)
            {
                DebugP_log("cpsw_min: port 1 link %s, speed code %u\r\n",
                           (newLink != 0U) ? "up" : "down", gCpsw.port[0].speed);
                link = newLink;
            }
            nextPoll = now + CPSW_MIN_POLL_US;
        }

        if ((link != 0U) && (now >= nextTest))
        {
            sendTest(&gCpsw, seq++);
            nextTest = now + 1000000U;
        }

        /* Counters only once RX has been quiet for 1 s: the SDK UART log
         * blocks until the line is out (about 0.4 ms), long enough for a
         * line-rate burst to overrun the RX ring. */
        if (gCpsw.rxPkts != lastRxPkts)
        {
            lastRxPkts = gCpsw.rxPkts;
            lastRx = now;
        }
        if ((now >= nextLog) && ((now - lastRx) >= 1000000U))
        {
            DebugP_log("cpsw_min: tx %u rx %u rxErr %u\r\n",
                       gCpsw.txPkts, gCpsw.rxPkts, gCpsw.rxErrs);
            nextLog = now + 5000000U;
        }
    }
}
