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
 * cpsw_min_example: CPSW MAC port 1 (DP83867 on the AM243x EVM) with the
 * cpsw_min driver. Every received frame is sent back to its sender, and a
 * broadcast test frame goes out once a second while the link is up.
 * Counters are printed every 5 seconds.
 */

#include <string.h>
#include <kernel/dpl/ClockP.h>
#include <kernel/dpl/DebugP.h>
#include <kernel/dpl/SystemP.h>
#include <drivers/pinmux.h>
#include "cpsw_min.h"

#define TEST_ETHERTYPE      (0x88B5U)   /* IEEE local experimental */

/* RGMII1 + MDIO on the AM243x EVM (values from the Enet SysConfig output) */
#define RGMII_IN            (PIN_MODE(4) | PIN_INPUT_ENABLE | PIN_PULL_DISABLE)
static const Pinmux_PerCfg_t gCpswPins[] =
{
    { PIN_PRG0_PRU1_GPO19, PIN_MODE(4) | PIN_PULL_DISABLE },     /* MDC   */
    { PIN_PRG0_PRU1_GPO18, RGMII_IN },                           /* MDIO  */
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
    .port     = { { .macPort = 1U, .phyAddr = 0U } },
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
                DebugP_log("cpsw_min: port 1 link %s, speed code %u\r\n",
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
            DebugP_log("cpsw_min: tx %u rx %u rxErr %u\r\n",
                       gCpsw.txPkts, gCpsw.rxPkts, gCpsw.rxErrs);
            nextLog = now + 5000000U;
        }
    }
}
