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
#include <drivers/soc.h>
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
        DebugP_log("cpsw_min: U42 pins 0x%02x (CONN0_DET %u, RST0 %u, SEL1 %u, SEL2 %u)\r\n",
                   in, in & 1U, (in >> 1) & 1U, (in >> 3) & 1U,
                   (in >> 4) & 1U);
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

#if defined(CPSW_MIN_TESTER)

/*
 * Tester mode: instead of echoing, drive a far-end echo (another enet_min
 * example, or any peer that swaps the MACs and returns the frame) and print
 * the same "ECHO ..." result lines as the SOEM tester (ecat-echo,
 * examples/soem_slaveinfo/echo_test.c), so the bench parser works on both.
 * Frame format and test matrix are copied from there. TX uses the copying
 * getTxBuf/send path, RX recv/recvDone; nothing is printed while timing.
 */
#include <kernel/dpl/CycleCounterP.h>

#define ECHO_MAGIC          (0x4F484345U)   /* "ECHO" */
#define ECHO_HDR            (14U + 12U)     /* Ethernet + magic, seq, len */
#define ECHO_MAX_FRAME      (1514U)
#define ECHO_RTT_FRAMES     (2000U)
#define ECHO_RTT_TIMEOUT    (10000U)        /* us */
#define ECHO_TPUT_SECS      (5U)
#define ECHO_TPUT_TIMEOUT   (20000U)        /* us before an echo counts lost */
#define ECHO_MAX_WIN        (8U)
#define ECHO_HIST_BINS      (2048U)         /* 1 us bins */
#define ECHO_START_DELAY_US (2000000U)

typedef struct
{
    uint32_t ok, lost, bad, other, late, extraLen;
} EchoCounts;

static uint32_t gHist[ECHO_HIST_BINS];
static uint8_t  gDst[6] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU };
static uint32_t gDstKnown;
static uint32_t gCpuMhz;

static uint8_t patByte(uint32_t seq, uint32_t i)
{
    return (uint8_t)((seq * 7U) + i);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Build and send one frame; 1 if it went out */
static uint32_t sendFrame(uint32_t seq, uint32_t len)
{
    uint8_t *buf = CpswMin_getTxBuf(&gCpsw);
    uint32_t i;

    if (buf == NULL)
    {
        return 0U;
    }
    memcpy(buf, gDst, 6U);
    memcpy(&buf[6], gCpswCfg.macAddr, 6U);
    buf[12] = (uint8_t)(TEST_ETHERTYPE >> 8);
    buf[13] = (uint8_t)TEST_ETHERTYPE;
    put32(&buf[14], ECHO_MAGIC);
    put32(&buf[18], seq);
    put32(&buf[22], len);
    for (i = ECHO_HDR; i < len; i++)
    {
        buf[i] = patByte(seq, i);
    }
    return (CpswMin_send(&gCpsw, buf, len, MAC_PORT) == SystemP_SUCCESS) ? 1U : 0U;
}

/* Sequence number of an echo of ours, or -1 for anything else. *good is 1 if
 * every byte matches. The far end's MAC is learned from the first echo. */
static int32_t checkEcho(const uint8_t *rx, uint32_t rxLen, EchoCounts *c, uint32_t *good)
{
    uint32_t seq, len, i;

    *good = 0U;
    if ((rxLen < ECHO_HDR) || (memcmp(rx, gCpswCfg.macAddr, 6U) != 0) ||
        (rx[12] != (uint8_t)(TEST_ETHERTYPE >> 8)) || (rx[13] != (uint8_t)TEST_ETHERTYPE) ||
        (get32(&rx[14]) != ECHO_MAGIC))
    {
        c->other++;
        return -1;
    }
    if (gDstKnown == 0U)
    {
        memcpy(gDst, &rx[6], 6U);
        gDstKnown = 1U;
    }
    seq = get32(&rx[18]);
    len = get32(&rx[22]);
    if ((len > ECHO_MAX_FRAME) || (rxLen < len))
    {
        return (int32_t)seq;
    }
    if (rxLen > len)
    {
        c->extraLen++;
    }
    for (i = ECHO_HDR; i < len; i++)
    {
        if (rx[i] != patByte(seq, i))
        {
            return (int32_t)seq;
        }
    }
    *good = 1U;
    return (int32_t)seq;
}

/* Next received frame checked against the echo format: -2 if none pending */
static int32_t recvEcho(EchoCounts *c, uint32_t *good)
{
    uint32_t len, port;
    uint8_t *rx = CpswMin_recv(&gCpsw, &len, &port);
    int32_t seq;

    if (rx == NULL)
    {
        return -2;
    }
    seq = checkEcho(rx, len, c, good);
    CpswMin_recvDone(&gCpsw, rx);
    return seq;
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t x = v, y = (v + 1U) / 2U;

    while (y < x)
    {
        x = y;
        y = (x + (v / x)) / 2U;
    }
    return (uint32_t)x;
}

static uint32_t histPct(uint32_t n, uint32_t permille)
{
    uint32_t i, sum = 0U, want = ((n * permille) + 999U) / 1000U;

    for (i = 0U; i < ECHO_HIST_BINS; i++)
    {
        sum += gHist[i];
        if (sum >= want)
        {
            return i;
        }
    }
    return ECHO_HIST_BINS;
}

static int32_t rttTest(uint32_t len)
{
    EchoCounts c = { 0 };
    uint32_t n, good, ns, minNs = 0xFFFFFFFFU, maxNs = 0U;
    uint64_t sumNs = 0U, sumSq = 0U, avg, var;
    int32_t seq;

    memset(gHist, 0, sizeof(gHist));
    for (n = 0U; n < ECHO_RTT_FRAMES; n++)
    {
        uint64_t deadline;
        uint32_t c0, done = 0U;

        c0 = CycleCounterP_getCount32();
        if (sendFrame(n, len) == 0U)
        {
            c.lost++;
            continue;
        }
        deadline = ClockP_getTimeUsec() + ECHO_RTT_TIMEOUT;
        while (done == 0U)
        {
            seq = recvEcho(&c, &good);
            if (seq != -2)
            {
                uint32_t cyc = CycleCounterP_getCount32() - c0;

                if (seq == (int32_t)n)
                {
                    done = 1U;
                    if (good == 0U)
                    {
                        c.bad++;
                        continue;
                    }
                    c.ok++;
                    ns = (uint32_t)(((uint64_t)cyc * 1000U) / gCpuMhz);
                    minNs = (ns < minNs) ? ns : minNs;
                    maxNs = (ns > maxNs) ? ns : maxNs;
                    sumNs += ns;
                    sumSq += (uint64_t)ns * ns;
                    gHist[((ns / 1000U) < ECHO_HIST_BINS) ? (ns / 1000U) : (ECHO_HIST_BINS - 1U)]++;
                }
                else if ((seq >= 0) && ((uint32_t)seq < n))
                {
                    c.late++;
                }
                else if (seq >= 0)
                {
                    c.bad++;
                }
            }
            else if (ClockP_getTimeUsec() > deadline)
            {
                c.lost++;
                done = 1U;
            }
        }
    }

    avg = (c.ok != 0U) ? (sumNs / c.ok) : 0U;
    var = (c.ok != 0U) ? ((sumSq / c.ok) - (avg * avg)) : 0U;
    DebugP_log("ECHO RTT size=%u n=%u ok=%u lost=%u bad=%u other=%u late=%u extralen=%u "
               "min_ns=%u avg_ns=%u max_ns=%u sd_ns=%u p50_us=%u p99_us=%u p999_us=%u\r\n",
               (unsigned)len, (unsigned)ECHO_RTT_FRAMES, (unsigned)c.ok, (unsigned)c.lost,
               (unsigned)c.bad, (unsigned)c.other, (unsigned)c.late, (unsigned)c.extraLen,
               (unsigned)((c.ok != 0U) ? minNs : 0U), (unsigned)avg, (unsigned)maxNs,
               (unsigned)isqrt64(var), (unsigned)histPct(c.ok, 500U), (unsigned)histPct(c.ok, 990U),
               (unsigned)histPct(c.ok, 999U));
    return ((c.ok == ECHO_RTT_FRAMES) && (c.bad == 0U)) ? 0 : -1;
}

static int32_t tputTest(uint32_t len, uint32_t win)
{
    EchoCounts c = { 0 };
    uint64_t sentAt[ECHO_MAX_WIN];
    uint32_t next = 0U, oldest = 0U, good;
    uint64_t start, now, end, us;
    uint32_t fps, kbps, lineKbps;
    int32_t seq;

    start = ClockP_getTimeUsec();
    end = start + ((uint64_t)ECHO_TPUT_SECS * 1000000U);
    now = start;
    while ((now < end) || (oldest != next))
    {
        /* keep 'win' frames in flight until the time is up, then drain */
        if ((now < end) && ((next - oldest) < win))
        {
            if (sendFrame(next, len) != 0U)
            {
                sentAt[next % ECHO_MAX_WIN] = now;
                next++;
            }
        }
        seq = recvEcho(&c, &good);
        now = ClockP_getTimeUsec();
        if (seq != -2)
        {
            if ((seq >= (int32_t)oldest) && (seq < (int32_t)next))
            {
                c.lost += (uint32_t)seq - oldest;   /* skipped over */
                oldest = (uint32_t)seq + 1U;
                if (good != 0U)
                {
                    c.ok++;
                }
                else
                {
                    c.bad++;
                }
            }
            else if ((seq >= 0) && ((uint32_t)seq < oldest))
            {
                c.late++;
            }
            else if (seq >= 0)
            {
                c.bad++;
            }
        }
        else if ((oldest != next) && ((now - sentAt[oldest % ECHO_MAX_WIN]) > ECHO_TPUT_TIMEOUT))
        {
            c.lost++;
            oldest++;
        }
    }

    us = now - start;
    fps = (uint32_t)(((uint64_t)c.ok * 1000000U) / us);
    /* Ethernet payload bit rate, and line rate incl. FCS, preamble, IFG */
    kbps = (uint32_t)(((uint64_t)c.ok * len * 8U * 1000U) / us);
    lineKbps = (uint32_t)(((uint64_t)c.ok * (len + 24U) * 8U * 1000U) / us);
    DebugP_log("ECHO TPUT size=%u win=%u sent=%u ok=%u lost=%u bad=%u other=%u late=%u extralen=%u "
               "us=%u fps=%u kbps=%u line_kbps=%u\r\n",
               (unsigned)len, (unsigned)win, (unsigned)next, (unsigned)c.ok, (unsigned)c.lost,
               (unsigned)c.bad, (unsigned)c.other, (unsigned)c.late, (unsigned)c.extraLen,
               (unsigned)us, (unsigned)fps, (unsigned)kbps, (unsigned)lineKbps);
    return ((c.ok == next) && (c.bad == 0U) && (next != 0U)) ? 0 : -1;
}

/* Receive and discard for us microseconds (also lets the driver reap TX) */
static void drainFor(uint32_t us)
{
    EchoCounts c = { 0 };
    uint32_t good;
    uint64_t end = ClockP_getTimeUsec() + us;

    while (ClockP_getTimeUsec() < end)
    {
        (void)recvEcho(&c, &good);
    }
}

static void testerMain(void)
{
    static const uint32_t rttSizes[] = { 60U, 128U, 256U, 512U, 1024U, 1514U };
    static const uint32_t tputSizes[] = { 60U, 1514U };
    static const uint32_t wins[] = { 1U, 4U, 8U };
    static const uint32_t probeSizes[] = { 60U, 64U, 128U, 512U };
    uint32_t i, j, k, fails = 0U, echoed = 0U;

    gCpuMhz = (uint32_t)(SOC_getSelfCpuClk() / 1000000U);
    CycleCounterP_reset();

    while (CpswMin_poll(&gCpsw) == 0U)
    {
        ClockP_usleep(CPSW_MIN_POLL_US);
    }
    DebugP_log("cpsw_min: tester, link up, starting in 2 s\r\n");
    drainFor(ECHO_START_DELAY_US);
    DebugP_log("ECHO START cpu_mhz=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\r\n", (unsigned)gCpuMhz,
               gCpswCfg.macAddr[0], gCpswCfg.macAddr[1], gCpswCfg.macAddr[2],
               gCpswCfg.macAddr[3], gCpswCfg.macAddr[4], gCpswCfg.macAddr[5]);

    /* Probe: 5 frames at each of 4 sizes; the first goes to broadcast, the
     * far end's MAC is learned from its echo */
    {
        EchoCounts c = { 0 };
        uint32_t good, got, sendFail = 0U, rxAny = 0U;

        for (k = 0U; k < (sizeof(probeSizes) / sizeof(probeSizes[0])); k++)
        {
            got = 0U;
            for (i = 0U; i < 5U; i++)
            {
                uint64_t deadline;
                int32_t seq;

                if (sendFrame(0x7FFF0000U + (k * 16U) + i, probeSizes[k]) == 0U)
                {
                    sendFail++;
                }
                deadline = ClockP_getTimeUsec() + ECHO_RTT_TIMEOUT;
                while (ClockP_getTimeUsec() < deadline)
                {
                    seq = recvEcho(&c, &good);
                    if (seq != -2)
                    {
                        rxAny++;
                    }
                    if (seq >= 0)
                    {
                        got++;
                    }
                }
            }
            DebugP_log("ECHO PROBE size=%u sent=5 echoed=%u\r\n", (unsigned)probeSizes[k], (unsigned)got);
            echoed += got;
        }
        DebugP_log("ECHO PROBE DIAG sendFail=%u rxAny=%u\r\n", (unsigned)sendFail, (unsigned)rxAny);
        DebugP_log("ECHO PROBE sent=20 echoed=%u other=%u\r\n", (unsigned)echoed, (unsigned)c.other);
    }

    if (echoed == 0U)
    {
        DebugP_log("ECHO RESULT FAIL (no echo from the far end)\r\n");
    }
    else
    {
        for (i = 0U; i < (sizeof(rttSizes) / sizeof(rttSizes[0])); i++)
        {
            fails += (rttTest(rttSizes[i]) != 0) ? 1U : 0U;
        }
        for (i = 0U; i < (sizeof(tputSizes) / sizeof(tputSizes[0])); i++)
        {
            for (j = 0U; j < (sizeof(wins) / sizeof(wins[0])); j++)
            {
                fails += (tputTest(tputSizes[i], wins[j]) != 0) ? 1U : 0U;
            }
        }
        DebugP_log("ECHO RESULT %s (%u of %u tests failed)\r\n", (fails == 0U) ? "PASS" : "FAIL",
                   (unsigned)fails,
                   (unsigned)((sizeof(rttSizes) / sizeof(rttSizes[0])) +
                              ((sizeof(tputSizes) / sizeof(tputSizes[0])) * (sizeof(wins) / sizeof(wins[0])))));
    }

    for (;;)
    {
        drainFor(1000000U);     /* idle, silent */
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
#if defined(CPSW_MIN_TESTER)
    testerMain();       /* does not return */
#endif
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
                DebugP_log("cpsw_min: port %u link %s, speed code %u\r\n", MAC_PORT,
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
