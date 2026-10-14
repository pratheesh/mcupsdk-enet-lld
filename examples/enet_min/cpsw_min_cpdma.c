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
 * cpsw_min for the AM261x: CPSW3G with the CPDMA instead of PKTDMA.
 *
 * Both directions use channel 0, polled, with singly linked CPPI 3.0
 * descriptors that the hardware walks. The descriptors sit in the uncached
 * ENET_CPPI_DESC section because the CPDMA updates them in place (flags,
 * OWNER, EOQ) while software links new ones; packet buffers stay cached with
 * explicit write-back and invalidate, as in the AM243x driver.
 *
 * Queue protocol (both directions): a descriptor is appended by pointing the
 * tail's next at it. If the hardware had already read the tail's next as 0
 * it stops after the tail and sets EOQ; that is only seen when the tail is
 * reaped, so the reap path restarts the channel by writing HDP with the
 * tail's next. HDP is written directly only when the queue is empty, i.e.
 * when the channel is known to be stopped.
 *
 * MDIO is never touched: MAC ports are fixed-speed full-duplex MII links
 * (CpswMin_PortCfg.fixedSpeed) and CpswMin_poll() always reports link up.
 */

#include <stddef.h>
#include <string.h>
#include <kernel/dpl/CacheP.h>
#include <kernel/dpl/ClockP.h>
#include <kernel/dpl/SystemP.h>
#include <drivers/soc.h>
#include <drivers/hw_include/cslr_soc.h>
#include "cpsw_min.h"
#include "cpsw_min_cpdma_regs.h"

#define NUM_TX                  (CPSW_MIN_NUM_TX)
#define NUM_RX                  (CPSW_MIN_NUM_RX)
#define SPIN_MAX                (10000U)

#define REG32(a)                (*(volatile uint32_t *)(uintptr_t)(a))

static CpswMin_Desc gTxDesc[NUM_TX] __attribute__((section(".bss:ENET_CPPI_DESC"), aligned(CPSW_MIN_DESC_SIZE)));
static CpswMin_Desc gRxDesc[NUM_RX] __attribute__((section(".bss:ENET_CPPI_DESC"), aligned(CPSW_MIN_DESC_SIZE)));

/* ------------------------------------------------------------------------ */
/* Small helpers (kept static so each driver file stands alone)             */
/* ------------------------------------------------------------------------ */

static void wbCache(const void *p, uint32_t size)
{
    CacheP_wb((void *)p, size, CacheP_TYPE_ALLD);
}

static void invCache(const void *p, uint32_t size)
{
    CacheP_inv((void *)p, size, CacheP_TYPE_ALLD);
}

/* Descriptor writes must be visible before the register or link write that
 * hands them to the hardware */
static void barrier(void)
{
    __asm__ volatile ("dsb" ::: "memory");
}

/* Poll until (reg & mask) == val; SystemP_FAILURE on timeout */
static int32_t waitReg(uint32_t addr, uint32_t mask, uint32_t val)
{
    uint32_t i;

    for (i = 0U; i < SPIN_MAX; i++)
    {
        if ((REG32(addr) & mask) == val)
        {
            return SystemP_SUCCESS;
        }
        ClockP_usleep(1U);
    }
    return SystemP_FAILURE;
}

/* ---- TX ----------------------------------------------------------------- */

static void rxQueue(CpswMin_Obj *h, uint32_t idx);

/* Reap sent descriptors; restart the channel if it stopped before a
 * descriptor that was linked behind it. An RX buffer lent by CpswMin_sendRx()
 * goes back to the RX queue now that its frame is out. */
static void txReap(CpswMin_Obj *h)
{
    while (h->txRd != h->txWr)
    {
        uint32_t idx = h->txQ[h->txRd & (NUM_TX - 1U)];
        CpswMin_Desc *d = &gTxDesc[idx];
        uint32_t flags = d->flags;

        if ((flags & DESC_OWNER) != 0U)
        {
            break;
        }
        h->txRd++;
        if (((flags & DESC_EOQ) != 0U) && (d->next != 0U))
        {
            REG32(CPDMA_TX_HDP) = d->next;
        }
        if (h->txRx[idx] != 0U)
        {
            uint32_t r = (uint32_t)h->txRx[idx] - 1U;

            h->txRx[idx] = 0U;
            rxQueue(h, r);
        }
        h->txFree[h->txFreeCnt++] = (uint8_t)idx;
    }
}

/* Fill TX descriptor idx for buf/len and append it to the TX queue. Padding
 * to 60 B is done by the callers. */
static void txQueue(CpswMin_Obj *h, uint32_t idx, uint8_t *buf, uint32_t len, uint32_t macPort)
{
    CpswMin_Desc *d = &gTxDesc[idx];

    d->next      = 0U;
    d->bufPtr    = (uint32_t)(uintptr_t)buf;
    d->bufOffLen = len;
    d->flags     = DESC_SOP | DESC_EOP | DESC_OWNER | DESC_TO_PORT_EN |
                   (macPort << DESC_PORT_SHIFT) | len;  /* directed to MAC port */
    wbCache(buf, len);
    barrier();
    if (h->txRd == h->txWr)
    {
        REG32(CPDMA_TX_HDP) = (uint32_t)(uintptr_t)d;   /* queue empty: stopped */
    }
    else
    {
        gTxDesc[h->txQ[(h->txWr - 1U) & (NUM_TX - 1U)]].next = (uint32_t)(uintptr_t)d;
    }
    h->txQ[h->txWr++ & (NUM_TX - 1U)] = (uint8_t)idx;
    h->txPkts++;
}

/* ---- RX ----------------------------------------------------------------- */

/* Reset a descriptor and hand it to the hardware */
static void rxQueue(CpswMin_Obj *h, uint32_t idx)
{
    CpswMin_Desc *d = &gRxDesc[idx];
    CpswMin_Desc *tail = &gRxDesc[h->rxQ[(h->rxWr - 1U) & (NUM_RX - 1U)]];
    uint32_t wasEmpty = (h->rxRd == h->rxWr) ? 1U : 0U;

    d->next      = 0U;
    d->bufPtr    = (uint32_t)(uintptr_t)h->rxBuf[idx];
    d->bufOffLen = CPSW_MIN_BUF_SIZE;
    d->flags     = DESC_OWNER;
    invCache(h->rxBuf[idx], CPSW_MIN_BUF_SIZE);
    barrier();
    h->rxQ[h->rxWr++ & (NUM_RX - 1U)] = (uint8_t)idx;
    if (wasEmpty != 0U)
    {
        REG32(CPDMA_RX_HDP) = (uint32_t)(uintptr_t)d;   /* queue empty: stopped */
    }
    else
    {
        tail->next = (uint32_t)(uintptr_t)d;
    }
}

/* ---- MAC ports ---------------------------------------------------------- */

static void macPortReset(uint32_t mp)
{
    /* Idle the MAC (finish any frame), then soft reset it */
    REG32(CPSW_PN_MAC_CONTROL(mp)) |= MACCTL_CMD_IDLE;
    (void)waitReg(CPSW_PN_MAC_STATUS(mp), MACSTS_IDLE, MACSTS_IDLE);
    REG32(CPSW_PN_MAC_RESET(mp)) = 1U;
    (void)waitReg(CPSW_PN_MAC_RESET(mp), 1U, 0U);
    REG32(CPSW_PN_MAC_CONTROL(mp)) = 0U;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int32_t CpswMin_open(CpswMin_Obj *h, const CpswMin_Cfg *cfg)
{
    uint32_t i, p;
    int32_t status = SystemP_SUCCESS;

    memset(&h->txFree, 0, sizeof(*h) - offsetof(CpswMin_Obj, txFree));
    h->numPorts = (cfg->numPorts > CPSW_MIN_MAX_PORTS) ? CPSW_MIN_MAX_PORTS : cfg->numPorts;

    /* MII only, no MDIO: a fixed 10/100 full-duplex link per port */
    for (p = 0U; p < h->numPorts; p++)
    {
        uint8_t fixed = cfg->port[p].fixedSpeed;
        if ((fixed != CPSW_MIN_FIXED(CPSW_MIN_SPEED_10)) && (fixed != CPSW_MIN_FIXED(CPSW_MIN_SPEED_100)))
        {
            status = SystemP_FAILURE;
        }
    }

    /* Clocks as the Enet SysConfig output sets them: CPTS and the CPSW
     * 5/50/250 MHz clock mux (no gate); the CPPI clock needs no setup */
    if (status == SystemP_SUCCESS)
    {
        status  = SOC_moduleClockEnable(SOC_RcmPeripheralId_CPTS, 1U);
        status |= SOC_moduleSetClockFrequency(SOC_RcmPeripheralId_CPTS,
                                              SOC_RcmPeripheralClockSource_SYS_CLK, 250000000U);
        status |= SOC_moduleSetClockFrequency(SOC_RcmPeripheralId_CPSW_5_50_250,
                                              SOC_RcmPeripheralClockSource_DPLL_CORE_HSDIV0_CLKOUT1,
                                              500000000U);
    }

    /* MAC interface select (MII) in MSS_CTRL; the partition is left unlocked
     * like the Enet LLD does */
    if (status == SystemP_SUCCESS)
    {
        SOC_controlModuleUnlockMMR(SOC_DOMAIN_ID_MAIN, MSS_CTRL_PARTITION0);
        for (p = 0U; p < h->numPorts; p++)
        {
            REG32(MSS_CPSW_CONTROL) &= ~MSS_PORT_MODE_MASK(cfg->port[p].macPort);
        }
    }

    /* A reload keeps a running CPSW: stop the MACs, then reset the CPDMA,
     * which also clears the HDP/CP pointers into the previous image */
    if (status == SystemP_SUCCESS)
    {
        for (p = 0U; p < h->numPorts; p++)
        {
            macPortReset(cfg->port[p].macPort);
        }
        REG32(CPDMA_SOFT_RESET) = 1U;
        status = waitReg(CPDMA_SOFT_RESET, 1U, 0U);
    }

    /* Host port, ALE (bypass: everything to host, host TX is directed) */
    if (status == SystemP_SUCCESS)
    {
        REG32(ALE_CTRL) = ALE_CTL_ENABLE | ALE_CTL_CLEAR | ALE_CTL_BYPASS;
        REG32(ALE_PORTCTL(0U)) = ALE_PORT_FORWARD;
        REG32(CPSW_P0_RX_MAXLEN) = CPSW_MIN_MTU;
        REG32(CPSW_CTRL) = CONTROL_P0_RX_PAD | CONTROL_P0_TX_CRC_REMOVE | CONTROL_P0_ENABLE;

        for (p = 0U; p < h->numPorts; p++)
        {
            const uint8_t *m = cfg->macAddr;
            uint32_t mp = cfg->port[p].macPort;

            h->port[p].macPort = (uint8_t)mp;
            h->port[p].phyAddr = cfg->port[p].phyAddr;
            h->port[p].linkUp = 1U;
            h->port[p].speed = (uint8_t)(cfg->port[p].fixedSpeed - 1U);
            h->port[p].fullDuplex = 1U;
            REG32(ALE_PORTCTL(mp)) = ALE_PORT_FORWARD;
            REG32(CPSW_PN_RX_MAXLEN(mp)) = CPSW_MIN_MTU + 4U;
            REG32(CPSW_PN_SA_H(mp)) = (uint32_t)m[0] | ((uint32_t)m[1] << 8) |
                                      ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
            REG32(CPSW_PN_SA_L(mp)) = (uint32_t)m[4] | ((uint32_t)m[5] << 8);
        }

        /* TX descriptors: buffers on the free stack */
        memset(gTxDesc, 0, sizeof(gTxDesc));
        for (i = 0U; i < NUM_TX; i++)
        {
            h->txFree[i] = (uint8_t)i;
        }
        h->txFreeCnt = NUM_TX;

        /* DMA on, then the RX queue; the MACs come up last so that no frame
         * arrives before there is a descriptor for it */
        REG32(CPDMA_RX_BUF_OFFSET) = 0U;
        REG32(CPDMA_TX_CONTROL) = 1U;
        REG32(CPDMA_RX_CONTROL) = 1U;
        memset(gRxDesc, 0, sizeof(gRxDesc));
        for (i = 0U; i < NUM_RX; i++)
        {
            rxQueue(h, i);
        }
        for (p = 0U; p < h->numPorts; p++)
        {
            REG32(CPSW_PN_MAC_CONTROL(cfg->port[p].macPort)) = MACCTL_GMII_EN | MACCTL_FULLDUPLEX;
        }
    }
    return status;
}

uint32_t CpswMin_poll(CpswMin_Obj *h)
{
    uint32_t p, mask = 0U;
    uint32_t err = REG32(CPDMA_DMASTATUS) & DMASTATUS_HOST_ERR;

    txReap(h);
    if (err != 0U)
    {
        h->dmaErr = err;
    }
    /* Fixed links: there is no PHY status to follow */
    for (p = 0U; p < h->numPorts; p++)
    {
        mask |= (uint32_t)h->port[p].linkUp << p;
    }
    return mask;
}

uint8_t *CpswMin_getTxBuf(CpswMin_Obj *h)
{
    txReap(h);
    return (h->txFreeCnt != 0U) ? h->txBuf[h->txFree[--h->txFreeCnt]] : NULL;
}

int32_t CpswMin_send(CpswMin_Obj *h, uint8_t *buf, uint32_t len, uint32_t macPort)
{
    uint32_t idx = (uint32_t)(buf - h->txBuf[0]) / CPSW_MIN_BUF_SIZE;

    if ((idx >= NUM_TX) || (len > CPSW_MIN_BUF_SIZE))
    {
        return SystemP_FAILURE;
    }
    if (len < 60U)
    {
        memset(&buf[len], 0, 60U - len);
        len = 60U;
    }
    txReap(h);
    txQueue(h, idx, buf, len, macPort);
    return SystemP_SUCCESS;
}

uint8_t *CpswMin_recv(CpswMin_Obj *h, uint32_t *len, uint32_t *macPort)
{
    uint8_t *buf = NULL;

    while ((buf == NULL) && (h->rxRd != h->rxWr))
    {
        uint32_t idx = h->rxQ[h->rxRd & (NUM_RX - 1U)];
        CpswMin_Desc *d = &gRxDesc[idx];
        uint32_t flags = d->flags;

        if ((flags & DESC_OWNER) != 0U)
        {
            break;
        }
        h->rxRd++;
        if (((flags & DESC_EOQ) != 0U) && (d->next != 0U))
        {
            REG32(CPDMA_RX_HDP) = d->next;              /* stopped before the link */
        }
        if ((flags & DESC_RX_ERR) != 0U)
        {
            h->rxErrs++;
            rxQueue(h, idx);
        }
        else
        {
            buf = h->rxBuf[idx];
            *len = flags & DESC_PKTLEN_MASK;
            *macPort = DESC_RX_FROM_PORT(flags);
            invCache(buf, *len);
            h->rxPkts++;
        }
    }
    if (buf == NULL)
    {
        txReap(h);      /* RX idle: reclaim TX, restart it, return lent RX buffers */
    }
    return buf;
}

void CpswMin_recvDone(CpswMin_Obj *h, uint8_t *buf)
{
    rxQueue(h, (uint32_t)(buf - h->rxBuf[0]) / CPSW_MIN_BUF_SIZE);
}

/* Zero copy: a TX descriptor is pointed at the RX buffer itself (the TX
 * descriptor keeps its own, unused, TX buffer; txRx[] records the lend). The
 * RX buffer re-enters the RX queue from txReap() once the hardware has sent
 * it, so the RX descriptor is re-initialised and re-linked at the RX tail
 * only after the frame is out. */
int32_t CpswMin_sendRx(CpswMin_Obj *h, uint8_t *buf, uint32_t len, uint32_t macPort)
{
    uint32_t r = (uint32_t)(buf - h->rxBuf[0]) / CPSW_MIN_BUF_SIZE;
    uint32_t i;

    if (h->txFreeCnt == 0U)
    {
        txReap(h);      /* normally done by CpswMin_recv() when RX is idle */
    }
    if ((r >= NUM_RX) || (len > CPSW_MIN_BUF_SIZE) || (h->txFreeCnt == 0U))
    {
        return SystemP_FAILURE;
    }
    if (len < 60U)
    {
        memset(&buf[len], 0, 60U - len);
        len = 60U;
    }
    i = h->txFree[--h->txFreeCnt];
    h->txRx[i] = (uint8_t)(r + 1U);
    txQueue(h, i, buf, len, macPort);
    return SystemP_SUCCESS;
}
