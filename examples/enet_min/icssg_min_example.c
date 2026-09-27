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
 * icssg_min_example: PRU_ICSSG1 MAC port 1 (DP83869 at MDIO address 15 on
 * the AM243x EVM) with the icssg_min driver. Every received frame is sent
 * back to its sender, and a broadcast test frame goes out once a second
 * while the link is up. Counters are printed every 5 seconds.
 *
 * Build with -DICSSG_MIN_SLICES=1 so only the slice 0 firmware is linked.
 */

#include <string.h>
#include <kernel/dpl/ClockP.h>
#include <kernel/dpl/DebugP.h>
#include <kernel/dpl/SystemP.h>
#include <drivers/pinmux.h>
#include "icssg_min.h"

#define TEST_ETHERTYPE      (0x88B5U)   /* IEEE local experimental */

/* ICSSG1 MDIO + RGMII1 on the AM243x EVM (values from the Enet SysConfig
 * output). RGMII2 would be the same pins on PRG1_PRU1. */
#define RGMII_IN            (PIN_MODE(2) | PIN_INPUT_ENABLE | PIN_PULL_DISABLE)
static const Pinmux_PerCfg_t gIcssgPins[] =
{
    { PIN_PRG1_MDIO0_MDC,  PIN_MODE(0) | PIN_PULL_DISABLE },                     /* MDC   */
    { PIN_PRG1_MDIO0_MDIO, PIN_MODE(0) | PIN_INPUT_ENABLE | PIN_PULL_DISABLE },  /* MDIO  */
    { PIN_PRG1_PRU0_GPO0,  RGMII_IN },                                           /* RD0   */
    { PIN_PRG1_PRU0_GPO1,  RGMII_IN },                                           /* RD1   */
    { PIN_PRG1_PRU0_GPO2,  RGMII_IN },                                           /* RD2   */
    { PIN_PRG1_PRU0_GPO3,  RGMII_IN },                                           /* RD3   */
    { PIN_PRG1_PRU0_GPO6,  RGMII_IN },                                           /* RXC   */
    { PIN_PRG1_PRU0_GPO4,  RGMII_IN },                                           /* RX_CTL*/
    { PIN_PRG1_PRU0_GPO11, RGMII_IN },                                           /* TD0   */
    { PIN_PRG1_PRU0_GPO12, RGMII_IN },                                           /* TD1   */
    { PIN_PRG1_PRU0_GPO13, RGMII_IN },                                           /* TD2   */
    { PIN_PRG1_PRU0_GPO14, RGMII_IN },                                           /* TD3   */
    { PIN_PRG1_PRU0_GPO16, RGMII_IN },                                           /* TXC   */
    { PIN_PRG1_PRU0_GPO15, RGMII_IN },                                           /* TX_CTL*/
    { PINMUX_END, 0U }
};

static IcssgMin_Obj gIcssg;

static const IcssgMin_Cfg gIcssgCfg =
{
    .icssg   = 1U,
    .slice   = 0U,
    .phyAddr = 15U,
    .macAddr = { 0x02U, 0x00U, 0x00U, 0x00U, 0x16U, 0x01U },
};

static void sendTest(IcssgMin_Obj *h, uint32_t seq)
{
    uint8_t *buf = IcssgMin_getTxBuf(h);

    if (buf != NULL)
    {
        memset(buf, 0xFF, 6U);
        memcpy(&buf[6], gIcssgCfg.macAddr, 6U);
        buf[12] = (uint8_t)(TEST_ETHERTYPE >> 8);
        buf[13] = (uint8_t)TEST_ETHERTYPE;
        memcpy(&buf[14], &seq, sizeof(seq));
        (void)IcssgMin_send(h, buf, 64U);
    }
}

static void echo(IcssgMin_Obj *h, const uint8_t *rx, uint32_t len)
{
    uint8_t *buf = IcssgMin_getTxBuf(h);

    if (buf != NULL)
    {
        memcpy(buf, &rx[6], 6U);
        memcpy(&buf[6], gIcssgCfg.macAddr, 6U);
        memcpy(&buf[12], &rx[12], len - 12U);
        (void)IcssgMin_send(h, buf, len);
    }
}

void icssg_min_example_main(void *args)
{
    uint64_t now, nextPoll = 0U, nextTest = 0U, nextLog = 0U;
    uint32_t link = 0U, seq = 0U, len;
    uint8_t *rx;

    Pinmux_config(gIcssgPins, PINMUX_DOMAIN_ID_MAIN);

    if (IcssgMin_open(&gIcssg, &gIcssgCfg) != SystemP_SUCCESS)
    {
        DebugP_log("icssg_min: open failed\r\n");
        return;
    }
    DebugP_log("icssg_min: firmware running, waiting for link\r\n");

    for (;;)
    {
        now = ClockP_getTimeUsec();

        while ((rx = IcssgMin_recv(&gIcssg, &len)) != NULL)
        {
            if (len >= 14U)
            {
                echo(&gIcssg, rx, len);
            }
            IcssgMin_recvDone(&gIcssg, rx);
        }

        if (now >= nextPoll)
        {
            uint32_t newLink = IcssgMin_poll(&gIcssg);
            if (newLink != link)
            {
                DebugP_log("icssg_min: port 1 link %s, speed code %u\r\n",
                           (newLink != 0U) ? "up" : "down", gIcssg.speed);
                link = newLink;
            }
            nextPoll = now + 100000U;
        }

        if ((link != 0U) && (now >= nextTest))
        {
            sendTest(&gIcssg, seq++);
            nextTest = now + 1000000U;
        }

        if (now >= nextLog)
        {
            DebugP_log("icssg_min: tx %u rx %u rxErr %u\r\n",
                       gIcssg.txPkts, gIcssg.rxPkts, gIcssg.rxErrs);
            nextLog = now + 5000000U;
        }
    }
}
