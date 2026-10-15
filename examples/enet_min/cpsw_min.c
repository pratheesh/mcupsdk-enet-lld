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

#include <stddef.h>
#include <string.h>
#include <kernel/dpl/CacheP.h>
#include <kernel/dpl/ClockP.h>
#include <kernel/dpl/SystemP.h>
#include <drivers/sciclient.h>
#include <drivers/hw_include/cslr_soc.h>
#include "cpsw_min.h"

/* ------------------------------------------------------------------------ */
/* SoC resources (AM64x/AM243x PKTDMA, r5fss0-0 default RM board config)    */
/* ------------------------------------------------------------------------ */

#define CPSW_BASE               (CSL_CPSW0_NUSS_BASE)
#define RINGRT_BASE             (CSL_DMASS0_PKTDMA_RINGRT_BASE)
#define TCHANRT_BASE            (CSL_DMASS0_PKTDMA_TCHANRT_BASE)
#define RCHANRT_BASE            (CSL_DMASS0_PKTDMA_RCHANRT_BASE)

#define CPSW_TX_CH              (16U)       /* mapped CPSW TX channel */
#define CPSW_TX_RING            (16U)
#define CPSW_RX_CH              (16U)       /* mapped CPSW RX channel */
#define CPSW_RX_FLOW            (17U)       /* flow 16 (ring 128) is default */
#define CPSW_RX_RING            (112U + CPSW_RX_FLOW)
#define CPSW_PSIL_TX            (0xC500U)
#define CPSW_PSIL_RX            (0x4500U)

#define NAV_PKTDMA              (TISCI_DEV_DMASS0_PKTDMA_0)
#define NAV_PSIL                (TISCI_DEV_DMASS0)

/* ------------------------------------------------------------------------ */
/* Registers                                                                */
/* ------------------------------------------------------------------------ */

#define REG32(a)                (*(volatile uint32_t *)(uintptr_t)(a))

/* PKTDMA ring / channel real-time registers */
#define RING_FDB(r)             (RINGRT_BASE + 0x0010U + ((r) * 0x2000U))
#define RING_RDB(r)             (RINGRT_BASE + 0x1010U + ((r) * 0x2000U))
#define RING_ROCC(r)            (RINGRT_BASE + 0x1018U + ((r) * 0x2000U))
#define CHRT_CTL(base, ch)      ((base) + ((ch) * 0x1000U))
#define CHRT_PEER8(base, ch)    ((base) + ((ch) * 0x1000U) + 0x220U)
#define CHRT_EN                 (0x80000000U)

/* CPSW3G subsystem */
#define CPSW_CONTROL            (CPSW_BASE + 0x20004U)
#define CPSW_P0_FLOW_ID_OFFSET  (CPSW_BASE + 0x21008U)
#define CPSW_P0_RX_MAXLEN       (CPSW_BASE + 0x21024U)
#define CPSW_PN(p)              (CPSW_BASE + 0x22000U + (((p) - 1U) * 0x1000U))
#define CPSW_PN_RX_MAXLEN(p)    (CPSW_PN(p) + 0x024U)
#define CPSW_PN_SA_L(p)         (CPSW_PN(p) + 0x308U)
#define CPSW_PN_SA_H(p)         (CPSW_PN(p) + 0x30CU)
#define CPSW_PN_MAC_CONTROL(p)  (CPSW_PN(p) + 0x330U)
#define ALE_CONTROL             (CPSW_BASE + 0x3E008U)
#define ALE_PORTCTL(n)          (CPSW_BASE + 0x3E040U + ((n) * 4U))
#define ALE_THREADMAPDEF        (CPSW_BASE + 0x3E134U)
#define MDIO_BASE               (CPSW_BASE + 0x00F00U)

#define CONTROL_P0_ENABLE       (1U << 2)
#define CONTROL_P0_TX_CRC_REMOVE (1U << 13)
#define CONTROL_P0_RX_PAD       (1U << 14)
#define MACCTL_FULLDUPLEX       (1U << 0)
#define MACCTL_GMII_EN          (1U << 5)
#define MACCTL_GIG              (1U << 7)
#define MACCTL_EXT_EN           (1U << 18)
#define ALE_CTL_ENABLE          (1U << 31)
#define ALE_CTL_CLEAR           (1U << 30)
#define ALE_CTL_BYPASS          (1U << 4)
#define ALE_PORT_FORWARD        (3U)

/* CTRL_MMR0: MAC port interface select */
#define MMR_ENET_CTRL(p)        (CSL_CTRL_MMR0_CFG0_BASE + 0x4044U + (((p) - 1U) * 4U))
#define MMR_LOCK1_KICK0         (CSL_CTRL_MMR0_CFG0_BASE + 0x5008U)
#define MMR_LOCK1_KICK1         (CSL_CTRL_MMR0_CFG0_BASE + 0x500CU)
#define ENET_CTRL_RGMII         (2U)

/* CPPI5 host descriptor */
#define DESC_HOST               (1U << 30)
#define DESC_EINFO              (1U << 29)
#define DESC_PSWCNT_16B         (4U << 22)
#define DESC_PKTLEN_MASK        (0x3FFFFFU)
#define DESC_PKTERR(x)          (((x) >> 28) & 0xFU)
#define DESC_WORD0              (DESC_HOST | DESC_EINFO | DESC_PSWCNT_16B)

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

static int32_t powerOn(uint32_t devId)
{
    uint32_t modState = 0U, resetState = 0U, ctxLoss = 0U;
    int32_t status;

    status = Sciclient_pmGetModuleState(devId, &modState, &resetState, &ctxLoss,
                                        SystemP_WAIT_FOREVER);
    if ((status == SystemP_SUCCESS) && (modState == TISCI_MSG_VALUE_DEVICE_HW_STATE_OFF))
    {
        status = Sciclient_pmSetModuleState(devId, TISCI_MSG_VALUE_DEVICE_SW_STATE_ON,
                                            TISCI_MSG_FLAG_AOP | TISCI_MSG_FLAG_DEVICE_RESET_ISO,
                                            SystemP_WAIT_FOREVER);
        if (status == SystemP_SUCCESS)
        {
            status = Sciclient_pmSetModuleRst(devId, 0U, SystemP_WAIT_FOREVER);
        }
    }
    return status;
}

/* ---- PKTDMA ------------------------------------------------------------- */

static int32_t ringInit(CpswMin_Ring *r, uint32_t num, uint64_t *mem, uint32_t cnt)
{
    struct tisci_msg_rm_ring_cfg_req  req;
    struct tisci_msg_rm_ring_cfg_resp resp;

    memset(mem, 0, cnt * sizeof(uint64_t));
    wbCache(mem, cnt * sizeof(uint64_t));
    r->mem = mem; r->num = num; r->cnt = cnt;
    r->wrIdx = 0U; r->rdIdx = 0U; r->occ = 0U;

    memset(&req, 0, sizeof(req));
    req.valid_params = TISCI_MSG_VALUE_RM_RING_ADDR_LO_VALID | TISCI_MSG_VALUE_RM_RING_ADDR_HI_VALID |
                       TISCI_MSG_VALUE_RM_RING_COUNT_VALID | TISCI_MSG_VALUE_RM_RING_MODE_VALID |
                       TISCI_MSG_VALUE_RM_RING_SIZE_VALID | TISCI_MSG_VALUE_RM_RING_ORDER_ID_VALID |
                       TISCI_MSG_VALUE_RM_RING_ASEL_VALID;
    req.nav_id   = NAV_PKTDMA;
    req.index    = (uint16_t)num;
    req.addr_lo  = (uint32_t)(uintptr_t)mem;
    req.addr_hi  = 0U;
    req.count    = cnt;
    req.mode     = TISCI_MSG_VALUE_RM_RING_MODE_RING;
    req.size     = TISCI_MSG_VALUE_RM_RING_SIZE_8B;
    req.order_id = 0U;
    req.asel     = 0U;
    return Sciclient_rmRingCfg(&req, &resp, SystemP_WAIT_FOREVER);
}

/* Queue one descriptor on the forward side of a dual ring */
static void ringPush(CpswMin_Ring *r, const void *desc)
{
    volatile uint64_t *slot = &r->mem[r->wrIdx];

    *slot = (uint64_t)(uintptr_t)desc;
    wbCache((const void *)slot, sizeof(uint64_t));
    r->wrIdx = (r->wrIdx + 1U) & (r->cnt - 1U);
    r->occ++;
    REG32(RING_FDB(r->num)) = 1U;
}

/* Dequeue one completed descriptor from the reverse side; NULL if none */
static void *ringPop(CpswMin_Ring *r)
{
    volatile uint64_t *slot;
    void *desc = NULL;

    if ((REG32(RING_ROCC(r->num)) & 0x1FFFFFU) != 0U)
    {
        slot = &r->mem[r->rdIdx];
        invCache((const void *)slot, sizeof(uint64_t));
        desc = (void *)(uintptr_t)(*slot);
        r->rdIdx = (r->rdIdx + 1U) & (r->cnt - 1U);
        r->occ--;
        REG32(RING_RDB(r->num)) = 0xFFU;    /* -1: ack one element */
    }
    return desc;
}

static int32_t txChInit(uint32_t ch)
{
    struct tisci_msg_rm_udmap_tx_ch_cfg_req  req;
    struct tisci_msg_rm_udmap_tx_ch_cfg_resp resp;

    memset(&req, 0, sizeof(req));
    req.valid_params = TISCI_MSG_VALUE_RM_UDMAP_CH_PAUSE_ON_ERR_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_ATYPE_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_CHAN_TYPE_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_FETCH_SIZE_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_CQ_QNUM_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_PRIORITY_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_QOS_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_ORDER_ID_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_SCHED_PRIORITY_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_TX_FILT_EINFO_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_TX_FILT_PSWORDS_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_TX_SUPR_TDPKT_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_TX_FDEPTH_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_BURST_SIZE_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_TX_CREDIT_COUNT_VALID;
    req.nav_id            = NAV_PKTDMA;
    req.index             = (uint16_t)ch;
    req.tx_atype          = TISCI_MSG_VALUE_RM_UDMAP_CH_ATYPE_PHYS;
    req.tx_chan_type      = TISCI_MSG_VALUE_RM_UDMAP_CH_TYPE_PACKET;
    req.tx_fetch_size     = 16U;    /* same as UDMA LLD default */
    req.txcq_qnum         = 0xFFFFU;
    req.tx_priority       = 4U;
    req.tx_qos            = 4U;
    req.tx_sched_priority = TISCI_MSG_VALUE_RM_UDMAP_CH_SCHED_PRIOR_MEDHIGH;
    req.tx_supr_tdpkt     = TISCI_MSG_VALUE_RM_UDMAP_TX_CH_SUPPRESS_TD_ENABLED;
    req.fdepth            = 192U;
    req.tx_burst_size     = TISCI_MSG_VALUE_RM_UDMAP_CH_BURST_SIZE_64_BYTES;
    return Sciclient_rmUdmapTxChCfg(&req, &resp, SystemP_WAIT_FOREVER);
}

static int32_t rxChInit(uint32_t ch)
{
    struct tisci_msg_rm_udmap_rx_ch_cfg_req  req;
    struct tisci_msg_rm_udmap_rx_ch_cfg_resp resp;

    memset(&req, 0, sizeof(req));
    req.valid_params = TISCI_MSG_VALUE_RM_UDMAP_CH_PAUSE_ON_ERR_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_ATYPE_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_CHAN_TYPE_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_FETCH_SIZE_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_CQ_QNUM_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_PRIORITY_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_QOS_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_ORDER_ID_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_SCHED_PRIORITY_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_RX_FLOWID_START_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_RX_FLOWID_CNT_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_RX_IGNORE_SHORT_VALID |
                       TISCI_MSG_VALUE_RM_UDMAP_CH_RX_IGNORE_LONG_VALID | TISCI_MSG_VALUE_RM_UDMAP_CH_BURST_SIZE_VALID;
    req.nav_id            = NAV_PKTDMA;
    req.index             = (uint16_t)ch;
    req.rx_fetch_size     = 16U;
    req.rxcq_qnum         = 0xFFFFU;
    req.rx_priority       = 4U;
    req.rx_qos            = 4U;
    req.rx_sched_priority = TISCI_MSG_VALUE_RM_UDMAP_CH_SCHED_PRIOR_MEDHIGH;
    req.rx_atype          = TISCI_MSG_VALUE_RM_UDMAP_CH_ATYPE_PHYS;
    req.rx_chan_type      = TISCI_MSG_VALUE_RM_UDMAP_CH_TYPE_PACKET;
    req.rx_ignore_short   = TISCI_MSG_VALUE_RM_UDMAP_RX_CH_PACKET_EXCEPTION;
    req.rx_ignore_long    = TISCI_MSG_VALUE_RM_UDMAP_RX_CH_PACKET_EXCEPTION;
    req.rx_burst_size     = TISCI_MSG_VALUE_RM_UDMAP_CH_BURST_SIZE_64_BYTES;
    return Sciclient_rmUdmapRxChCfg(&req, &resp, SystemP_WAIT_FOREVER);
}

static int32_t flowInit(uint32_t flow, uint32_t ring)
{
    struct tisci_msg_rm_udmap_flow_cfg_req              req;
    struct tisci_msg_rm_udmap_flow_cfg_resp             resp;
    struct tisci_msg_rm_udmap_flow_size_thresh_cfg_req  treq;
    struct tisci_msg_rm_udmap_flow_size_thresh_cfg_resp tresp;
    int32_t status;

    memset(&req, 0, sizeof(req));
    req.valid_params = 0x7FFFFU;    /* every field from EINFO_PRESENT to PS_LOCATION */
    req.nav_id              = NAV_PKTDMA;
    req.flow_index          = (uint16_t)flow;
    req.rx_einfo_present    = 1U;
    req.rx_psinfo_present   = 1U;
    req.rx_error_handling   = TISCI_MSG_VALUE_RM_UDMAP_RX_FLOW_ERR_DROP;
    req.rx_desc_type        = TISCI_MSG_VALUE_RM_UDMAP_RX_FLOW_DESC_HOST;
    req.rx_dest_qnum        = (uint16_t)ring;
    req.rx_src_tag_hi_sel   = TISCI_MSG_VALUE_RM_UDMAP_RX_FLOW_SRC_SELECT_FLOW_ID;
    req.rx_src_tag_lo_sel   = TISCI_MSG_VALUE_RM_UDMAP_RX_FLOW_SRC_SELECT_SRC_TAG;
    req.rx_dest_tag_hi_sel  = TISCI_MSG_VALUE_RM_UDMAP_RX_FLOW_DEST_SELECT_DEST_TAG_HI;
    req.rx_dest_tag_lo_sel  = TISCI_MSG_VALUE_RM_UDMAP_RX_FLOW_DEST_SELECT_DEST_TAG_LO;
    req.rx_fdq0_sz0_qnum    = (uint16_t)ring;
    req.rx_fdq1_qnum        = (uint16_t)ring;
    req.rx_fdq2_qnum        = (uint16_t)ring;
    req.rx_fdq3_qnum        = (uint16_t)ring;
    req.rx_ps_location      = TISCI_MSG_VALUE_RM_UDMAP_RX_FLOW_PS_END_PD;
    status = Sciclient_rmUdmapFlowCfg(&req, &resp, SystemP_WAIT_FOREVER);

    if (status == SystemP_SUCCESS)
    {
        memset(&treq, 0, sizeof(treq));
        treq.valid_params = 0x7FU;
        treq.nav_id            = NAV_PKTDMA;
        treq.flow_index        = (uint16_t)flow;
        treq.rx_size_thresh0   = (uint16_t)(CPSW_MIN_BUF_SIZE >> 5);
        treq.rx_size_thresh1   = (uint16_t)(CPSW_MIN_BUF_SIZE >> 5);
        treq.rx_size_thresh2   = (uint16_t)(CPSW_MIN_BUF_SIZE >> 5);
        treq.rx_fdq0_sz1_qnum  = (uint16_t)ring;
        treq.rx_fdq0_sz2_qnum  = (uint16_t)ring;
        treq.rx_fdq0_sz3_qnum  = (uint16_t)ring;
        treq.rx_size_thresh_en = 0U;
        status = Sciclient_rmUdmapFlowSizeThreshCfg(&treq, &tresp, SystemP_WAIT_FOREVER);
    }
    return status;
}

static int32_t psilPair(uint32_t src, uint32_t dst)
{
    struct tisci_msg_rm_psil_pair_req req;

    memset(&req, 0, sizeof(req));
    req.nav_id     = NAV_PSIL;
    req.src_thread = src;
    req.dst_thread = dst;
    return Sciclient_rmPsilPair(&req, SystemP_WAIT_FOREVER);
}

/* ---- MDIO (manual / bit-bang mode, erratum i2329 workaround) ----------- */

#define MDIO_MANUAL_IF          (MDIO_BASE + 0x30U)
#define MDIO_POLL               (MDIO_BASE + 0x34U)
#define MDIO_POLL_EN            (MDIO_BASE + 0x38U)
#define MDIO_PIN                (1U << 0)
#define MDIO_OE                 (1U << 1)
#define MDIO_MDCLK              (1U << 2)
#define MDIO_HALF_CYCLE_LOOPS   (200U)  /* ~1 MHz MDC or slower on R5F */

static void mdioDelay(void)
{
    volatile uint32_t i;
    for (i = 0U; i < MDIO_HALF_CYCLE_LOOPS; i++)
    {
    }
}

static void mdioSet(uint32_t mask, uint32_t on)
{
    uint32_t v = REG32(MDIO_MANUAL_IF);
    REG32(MDIO_MANUAL_IF) = (on != 0U) ? (v | mask) : (v & ~mask);
}

static void mdioClock(void)
{
    mdioSet(MDIO_MDCLK, 0U);
    mdioDelay();
    mdioSet(MDIO_MDCLK, 1U);
    mdioDelay();
}

static void mdioSend(uint32_t msb, uint32_t val)
{
    uint32_t i;
    for (i = msb; i != 0U; i >>= 1)
    {
        mdioSet(MDIO_PIN, val & i);
        mdioClock();
    }
}

static void mdioInit(void)
{
    REG32(MDIO_POLL_EN) = 0U;
    REG32(MDIO_POLL) |= (1U << 31) | (1U << 30);    /* manual mode */
}

static void mdioFrameStart(uint32_t op, uint32_t phy, uint32_t reg)
{
    mdioSet(MDIO_MDCLK, 0U);
    mdioSet(MDIO_OE, 1U);
    mdioSend(0x80000000U, 0xFFFFFFFFU);     /* preamble */
    mdioSend(0x8U, op);                     /* start + opcode */
    mdioSend(0x10U, phy);
    mdioSend(0x10U, reg);
}

static void mdioFrameEnd(void)
{
    mdioSet(MDIO_MDCLK, 0U);
    mdioDelay();
    mdioClock();
}

static uint16_t mdioRead(uint32_t phy, uint32_t reg)
{
    uint32_t i, ack;
    uint16_t val = 0U;

    mdioFrameStart(0x6U, phy, reg);
    mdioSet(MDIO_OE, 0U);                   /* turnaround */
    mdioClock();
    ack = REG32(MDIO_MANUAL_IF) & MDIO_PIN;
    mdioClock();
    for (i = 0x8000U; i != 0U; i >>= 1)
    {
        if ((REG32(MDIO_MANUAL_IF) & MDIO_PIN) != 0U)
        {
            val |= (uint16_t)i;
        }
        mdioClock();
    }
    mdioFrameEnd();
    return (ack == 0U) ? val : 0xFFFFU;
}

static void mdioWrite(uint32_t phy, uint32_t reg, uint16_t val)
{
    mdioFrameStart(0x5U, phy, reg);
    mdioSend(0x2U, 0x2U);                   /* turnaround "10" */
    mdioSend(0x8000U, val);
    mdioSet(MDIO_OE, 0U);
    mdioFrameEnd();
}

/* ---- PHY (TI DP83867, as on the AM243x EVM CPSW port 1) ---------------- */

#define PHY_BMCR                (0x00U)
#define PHY_BMSR                (0x01U)
#define PHY_MMD_CR              (0x0DU)
#define PHY_MMD_DR              (0x0EU)
#define PHY_PHYCR               (0x10U)
#define PHY_PHYSTS              (0x11U)
#define PHY_CFG3                (0x1EU)
#define PHY_CTRL                (0x1FU)
#define DP_FLDTHRCFG            (0x2EU)
#define DP_RGMIICTL             (0x32U)
#define DP_VTMCFG               (0x53U)
#define DP_STRAPSTS2            (0x6FU)
#define DP_RGMIIDCTL            (0x86U)
#define DP_LOOPCR               (0xFEU)
#define DP_DSPFFECFG            (0x12CU)
#define DP_IOMUXCFG             (0x170U)

static uint16_t phyReadExt(uint32_t phy, uint32_t reg)
{
    mdioWrite(phy, PHY_MMD_CR, 0x001FU);
    mdioWrite(phy, PHY_MMD_DR, (uint16_t)reg);
    mdioWrite(phy, PHY_MMD_CR, 0x401FU);
    return mdioRead(phy, PHY_MMD_DR);
}

static void phyWriteExt(uint32_t phy, uint32_t reg, uint16_t val)
{
    mdioWrite(phy, PHY_MMD_CR, 0x001FU);
    mdioWrite(phy, PHY_MMD_DR, (uint16_t)reg);
    mdioWrite(phy, PHY_MMD_CR, 0x401FU);
    mdioWrite(phy, PHY_MMD_DR, val);
}

static void phyRmwExt(uint32_t phy, uint32_t reg, uint16_t mask, uint16_t val)
{
    phyWriteExt(phy, reg, (uint16_t)((phyReadExt(phy, reg) & ~mask) | (val & mask)));
}

static void phyRmw(uint32_t phy, uint32_t reg, uint16_t mask, uint16_t val)
{
    mdioWrite(phy, reg, (uint16_t)((mdioRead(phy, reg) & ~mask) | (val & mask)));
}

static void phyConfig(uint32_t phy)
{
    uint32_t tries = 0U;

    mdioWrite(phy, PHY_BMCR, 0x8000U);                  /* soft reset */
    while (((mdioRead(phy, PHY_BMCR) & 0x8000U) != 0U) && (tries++ < 1000U))
    {
        ClockP_usleep(100U);
    }
    phyRmwExt(phy, DP_VTMCFG, 0x000FU, 4U);             /* Viterbi idle thresh */
    phyRmwExt(phy, DP_DSPFFECFG, 0x03FFU, 0x0281U);     /* short-cable FFE */
    if ((phyReadExt(phy, DP_STRAPSTS2) & 0x0400U) != 0U)
    {
        phyRmwExt(phy, DP_FLDTHRCFG, 0x0007U, 1U);      /* FLD strap fix */
    }
    phyWriteExt(phy, DP_LOOPCR, 0xE721U);               /* normal (no loopback) */
    phyRmw(phy, PHY_CTRL, 0x4000U, 0x4000U);            /* SW restart */
    phyRmw(phy, PHY_PHYCR, 0x0060U, 0x0040U);           /* auto MDI-X */
    phyRmw(phy, PHY_CFG3, 0x0200U, 0x0200U);            /* robust auto MDI-X */
    phyRmwExt(phy, DP_RGMIICTL, 0x0083U, 0x0083U);      /* RGMII + TX/RX clk shift */
    phyWriteExt(phy, DP_RGMIIDCTL, 0x0007U);            /* TX 0.25 ns, RX 2.0 ns */
    phyRmw(phy, PHY_PHYCR, 0xC000U, 0x4000U);           /* TX FIFO depth 4 */
    phyRmwExt(phy, DP_IOMUXCFG, 0x001FU, 0x001FU);      /* 35 ohm output impedance */
    phyRmw(phy, PHY_BMCR, 0x1200U, 0x1200U);            /* autoneg enable+restart */
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int32_t CpswMin_open(CpswMin_Obj *h, const CpswMin_Cfg *cfg)
{
    uint32_t i, p;
    int32_t status;

    memset(&h->txRing, 0, sizeof(*h) - offsetof(CpswMin_Obj, txRing));
    h->numPorts = (cfg->numPorts > CPSW_MIN_MAX_PORTS) ? CPSW_MIN_MAX_PORTS : cfg->numPorts;

    status = powerOn(TISCI_DEV_CPSW0);

    /* MAC interface select (RGMII) in CTRL_MMR0, partition 1 */
    if (status == SystemP_SUCCESS)
    {
        REG32(MMR_LOCK1_KICK0) = 0x68EF3490U;
        REG32(MMR_LOCK1_KICK1) = 0xD172BC5AU;
        for (p = 0U; p < h->numPorts; p++)
        {
            uint32_t a = MMR_ENET_CTRL(cfg->port[p].macPort);
            REG32(a) = (REG32(a) & ~0x7U) | ENET_CTRL_RGMII;
        }
    }

    /* Host port, ALE (bypass: everything to host, host TX is directed) */
    if (status == SystemP_SUCCESS)
    {
        REG32(ALE_CONTROL) = ALE_CTL_ENABLE | ALE_CTL_CLEAR | ALE_CTL_BYPASS;
        REG32(ALE_PORTCTL(0U)) = ALE_PORT_FORWARD;
        REG32(ALE_THREADMAPDEF) = (1U << 15) | 0U;      /* default thread 0 */
        REG32(CPSW_P0_FLOW_ID_OFFSET) = CPSW_RX_FLOW;
        REG32(CPSW_P0_RX_MAXLEN) = CPSW_MIN_MTU;
        REG32(CPSW_CONTROL) = CONTROL_P0_RX_PAD | CONTROL_P0_TX_CRC_REMOVE | CONTROL_P0_ENABLE;

        for (p = 0U; p < h->numPorts; p++)
        {
            const uint8_t *m = cfg->macAddr;
            uint32_t mp = cfg->port[p].macPort;

            h->port[p].macPort = (uint8_t)mp;
            h->port[p].phyAddr = cfg->port[p].phyAddr;
            REG32(ALE_PORTCTL(mp)) = ALE_PORT_FORWARD;
            REG32(CPSW_PN_RX_MAXLEN(mp)) = CPSW_MIN_MTU + 4U;
            REG32(CPSW_PN_SA_H(mp)) = (uint32_t)m[0] | ((uint32_t)m[1] << 8) |
                                      ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24);
            REG32(CPSW_PN_SA_L(mp)) = (uint32_t)m[4] | ((uint32_t)m[5] << 8);
            REG32(CPSW_PN_MAC_CONTROL(mp)) = 0U;        /* enabled on link up */
        }
    }

    /* DMA: rings, channels, flow, PSI-L pairing */
    if (status == SystemP_SUCCESS)
    {
        status  = ringInit(&h->txRing, CPSW_TX_RING, h->txRingMem, CPSW_MIN_NUM_TX);
        status |= ringInit(&h->rxRing, CPSW_RX_RING, h->rxRingMem, CPSW_MIN_NUM_RX);
        status |= txChInit(CPSW_TX_CH);
        status |= rxChInit(CPSW_RX_CH);
        status |= flowInit(CPSW_RX_FLOW, CPSW_RX_RING);
        status |= psilPair(CPSW_TX_CH + 0x1000U, CPSW_PSIL_TX);
        status |= psilPair(CPSW_PSIL_RX, CPSW_RX_CH + 0x9000U);
    }

    if (status == SystemP_SUCCESS)
    {
        /* TX descriptors: static fields; buffers on the free stack */
        for (i = 0U; i < CPSW_MIN_NUM_TX; i++)
        {
            CpswMin_Desc *d = &h->txDesc[i];
            d->pktInfo1  = 0U;
            d->pktInfo2  = 0U;
            d->bufPtr[0] = (uint32_t)(uintptr_t)h->txBuf[i];
            d->orgBufPtr[0] = d->bufPtr[0];
            d->orgBufLen = CPSW_MIN_BUF_SIZE;
            h->txFree[i] = (uint8_t)i;
        }
        h->txFreeCnt = CPSW_MIN_NUM_TX;

        /* RX descriptors: all handed to hardware */
        for (i = 0U; i < CPSW_MIN_NUM_RX; i++)
        {
            CpswMin_Desc *d = &h->rxDesc[i];
            d->descInfo  = DESC_WORD0;
            d->bufPtr[0] = (uint32_t)(uintptr_t)h->rxBuf[i];
            d->bufInfo1  = CPSW_MIN_BUF_SIZE;
            d->orgBufPtr[0] = d->bufPtr[0];
            d->orgBufLen = CPSW_MIN_BUF_SIZE;
            wbCache(d, sizeof(*d));
            invCache(h->rxBuf[i], CPSW_MIN_BUF_SIZE);
            ringPush(&h->rxRing, d);
        }

        /* Enable: TX peer then channel, RX channel then peer */
        REG32(CHRT_PEER8(TCHANRT_BASE, CPSW_TX_CH)) |= CHRT_EN;
        REG32(CHRT_CTL(TCHANRT_BASE, CPSW_TX_CH)) = CHRT_EN;
        REG32(CHRT_CTL(RCHANRT_BASE, CPSW_RX_CH)) = CHRT_EN;
        REG32(CHRT_PEER8(RCHANRT_BASE, CPSW_RX_CH)) |= CHRT_EN;

        mdioInit();
        for (p = 0U; p < h->numPorts; p++)
        {
            phyConfig(h->port[p].phyAddr);
        }
    }
    return status;
}

uint32_t CpswMin_poll(CpswMin_Obj *h)
{
    uint32_t p, mask = 0U;

    for (p = 0U; p < h->numPorts; p++)
    {
        CpswMin_Port *port = &h->port[p];
        uint32_t up;

        (void)mdioRead(port->phyAddr, PHY_BMSR);        /* latched-low link bit */
        up = ((mdioRead(port->phyAddr, PHY_BMSR) & 0x0004U) != 0U) ? 1U : 0U;
        if (up != port->linkUp)
        {
            uint32_t macCtl = 0U;
            if (up != 0U)
            {
                uint16_t sts = mdioRead(port->phyAddr, PHY_PHYSTS);
                port->speed = (uint8_t)((sts >> 14) & 0x3U);
                port->fullDuplex = (uint8_t)((sts >> 13) & 0x1U);
                macCtl = MACCTL_GMII_EN;
                macCtl |= (port->fullDuplex != 0U) ? MACCTL_FULLDUPLEX : 0U;
                macCtl |= (port->speed == CPSW_MIN_SPEED_1000) ? MACCTL_GIG : 0U;
                macCtl |= (port->speed == CPSW_MIN_SPEED_10) ? MACCTL_EXT_EN : 0U;
            }
            REG32(CPSW_PN_MAC_CONTROL(port->macPort)) = macCtl;
            port->linkUp = (uint8_t)up;
        }
        mask |= (uint32_t)port->linkUp << p;
    }
    return mask;
}

uint8_t *CpswMin_getTxBuf(CpswMin_Obj *h)
{
    CpswMin_Desc *d;

    while ((d = (CpswMin_Desc *)ringPop(&h->txRing)) != NULL)
    {
        h->txFree[h->txFreeCnt++] = (uint8_t)(d - h->txDesc);
    }
    return (h->txFreeCnt != 0U) ? h->txBuf[h->txFree[--h->txFreeCnt]] : NULL;
}

int32_t CpswMin_send(CpswMin_Obj *h, uint8_t *buf, uint32_t len, uint32_t macPort)
{
    uint32_t idx = (uint32_t)(buf - h->txBuf[0]) / CPSW_MIN_BUF_SIZE;
    CpswMin_Desc *d;

    if ((idx >= CPSW_MIN_NUM_TX) || (len > CPSW_MIN_BUF_SIZE))
    {
        return SystemP_FAILURE;
    }
    if (len < 60U)
    {
        memset(&buf[len], 0, 60U - len);
        len = 60U;
    }
    d = &h->txDesc[idx];
    d->descInfo  = DESC_WORD0 | len;
    d->srcDstTag = macPort;                     /* directed to MAC port */
    d->bufInfo1  = len;
    wbCache(buf, len);
    wbCache(d, sizeof(*d));
    ringPush(&h->txRing, d);
    h->txPkts++;
    return SystemP_SUCCESS;
}

uint8_t *CpswMin_recv(CpswMin_Obj *h, uint32_t *len, uint32_t *macPort)
{
    CpswMin_Desc *d;
    uint8_t *buf = NULL;

    while ((buf == NULL) && ((d = (CpswMin_Desc *)ringPop(&h->rxRing)) != NULL))
    {
        uint32_t idx = (uint32_t)(d - h->rxDesc);

        invCache(d, sizeof(*d));
        if (DESC_PKTERR(d->pktInfo1) != 0U)
        {
            h->rxErrs++;
            CpswMin_recvDone(h, h->rxBuf[idx]);
        }
        else
        {
            buf = h->rxBuf[idx];
            *len = d->descInfo & DESC_PKTLEN_MASK;
            *macPort = (d->srcDstTag >> 16) & 0xFFU;
            invCache(buf, *len);
            h->rxPkts++;
        }
    }
    return buf;
}

void CpswMin_recvDone(CpswMin_Obj *h, uint8_t *buf)
{
    uint32_t idx = (uint32_t)(buf - h->rxBuf[0]) / CPSW_MIN_BUF_SIZE;
    CpswMin_Desc *d = &h->rxDesc[idx];

    d->descInfo = DESC_WORD0;
    d->bufInfo1 = CPSW_MIN_BUF_SIZE;
    wbCache(d, sizeof(*d));
    invCache(buf, CPSW_MIN_BUF_SIZE);
    ringPush(&h->rxRing, d);
}
