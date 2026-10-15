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
#include "icssg_min.h"

/* TI dual-MAC firmware images (non-static arrays: include in one file only) */
#if (ICSSG_MIN_SLICES & 0x1U)
#include <networking/enet/core/src/per/firmware/icssg/dualmac/RX_PRU_SLICE0_bin.h>
#include <networking/enet/core/src/per/firmware/icssg/dualmac/RTU0_SLICE0_bin.h>
#include <networking/enet/core/src/per/firmware/icssg/dualmac/TX_PRU_SLICE0_bin.h>
#endif
#if (ICSSG_MIN_SLICES & 0x2U)
#include <networking/enet/core/src/per/firmware/icssg/dualmac/RX_PRU_SLICE1_bin.h>
#include <networking/enet/core/src/per/firmware/icssg/dualmac/RTU0_SLICE1_bin.h>
#include <networking/enet/core/src/per/firmware/icssg/dualmac/TX_PRU_SLICE1_bin.h>
#endif

/* Optional: ICSSG core clock in Hz, set before the firmware starts. Leave 0
 * to keep what the SBL / System_init programmed. */
#ifndef ICSSG_MIN_CORE_CLK_HZ
#define ICSSG_MIN_CORE_CLK_HZ   (0U)
#endif

/* ------------------------------------------------------------------------ */
/* SoC resources                                                            */
/* ------------------------------------------------------------------------ */

typedef struct
{
    uint32_t  devId;
    uintptr_t base;
    uint16_t  txCh[2], txRing[2], rxCh[2], rxFlow[2];
    uint16_t  psilTx[2], psilRx[2];
} IcssgMin_SocInfo;

/* Index = ICSSG instance; only ICSSG1 is filled in (EVM RGMII ports) */
static const IcssgMin_SocInfo gIcssgSoc[2] =
{
    [1] =
    {
        .devId  = TISCI_DEV_PRU_ICSSG1,
        .base   = CSL_PRU_ICSSG1_DRAM0_SLV_RAM_BASE,
        .txCh   = { 34U, 38U },     /* first mapped TX channel of each slice */
        .txRing = { 104U, 108U },
        .rxCh   = { 25U, 26U },
        .rxFlow = { 113U, 129U },   /* first flow after each default flow */
        .psilTx = { 0xC200U, 0xC204U },
        .psilRx = { 0x4200U, 0x4201U },
    },
};

typedef struct
{
    const uint32_t *img[3];     /* RX_PRU, RTU, TX_PRU */
    uint32_t        size[3];
} IcssgMin_Fw;

static const IcssgMin_Fw gIcssgFw[2] =
{
#if (ICSSG_MIN_SLICES & 0x1U)
    [0] = { { (const uint32_t *)RX_PRU_SLICE0_b00_DMac, (const uint32_t *)RTU0_SLICE0_b00_DMac,
              (const uint32_t *)TX_PRU_SLICE0_b00_DMac },
            { sizeof(RX_PRU_SLICE0_b00_DMac), sizeof(RTU0_SLICE0_b00_DMac),
              sizeof(TX_PRU_SLICE0_b00_DMac) } },
#endif
#if (ICSSG_MIN_SLICES & 0x2U)
    [1] = { { (const uint32_t *)RX_PRU_SLICE1_b00_DMac, (const uint32_t *)RTU0_SLICE1_b00_DMac,
              (const uint32_t *)TX_PRU_SLICE1_b00_DMac },
            { sizeof(RX_PRU_SLICE1_b00_DMac), sizeof(RTU0_SLICE1_b00_DMac),
              sizeof(TX_PRU_SLICE1_b00_DMac) } },
#endif
};

static uint8_t gIcssgSmemInit[2];   /* shared RAM cleared once per ICSSG */

#define RINGRT_BASE             (CSL_DMASS0_PKTDMA_RINGRT_BASE)
#define TCHANRT_BASE            (CSL_DMASS0_PKTDMA_TCHANRT_BASE)
#define RCHANRT_BASE            (CSL_DMASS0_PKTDMA_RCHANRT_BASE)
#define NAV_PKTDMA              (TISCI_DEV_DMASS0_PKTDMA_0)
#define NAV_PSIL                (TISCI_DEV_DMASS0)

/* ------------------------------------------------------------------------ */
/* Registers (offsets from the ICSSG base)                                  */
/* ------------------------------------------------------------------------ */

#define REG32(a)                (*(volatile uint32_t *)(uintptr_t)(a))
#define REG8(a)                 (*(volatile uint8_t *)(uintptr_t)(a))

#define RING_FDB(r)             (RINGRT_BASE + 0x0010U + ((r) * 0x2000U))
#define RING_RDB(r)             (RINGRT_BASE + 0x1010U + ((r) * 0x2000U))
#define RING_ROCC(r)            (RINGRT_BASE + 0x1018U + ((r) * 0x2000U))
#define CHRT_CTL(base, ch)      ((base) + ((ch) * 0x1000U))
#define CHRT_PEER8(base, ch)    ((base) + ((ch) * 0x1000U) + 0x220U)
#define CHRT_EN                 (0x80000000U)

#define ICSS_DRAM1              (0x02000U)
#define ICSS_SMEM               (0x10000U)
#define ICSS_SMEM_SIZE          (0x10000U)
#define ICSS_PRU_CTRL(s)        (0x22000U + ((s) * 0x2000U))    /* PDSP0/1 */
#define ICSS_RTU_CTRL(s)        (0x23000U + ((s) * 0x0800U))
#define ICSS_TXPRU_CTRL(s)      (0x25000U + ((s) * 0x0800U))
#define ICSS_PRU_IRAM(s)        (0x34000U + ((s) * 0x4000U))
#define ICSS_RTU_IRAM(s)        (0x04000U + ((s) * 0x2000U))
#define ICSS_TXPRU_IRAM(s)      (0x0A000U + ((s) * 0x2000U))
#define PRU_CTRL_ENABLE         (1U << 1)
#define PRU_C28_PTR             (0x28U)

#define ICSSCFG                 (0x26000U)
#define ICSSCFG_GPCFG0          (ICSSCFG + 0x08U)
#define ICSSCFG_GPCFG1          (ICSSCFG + 0x0CU)
#define ICSSCFG_IEPCLK          (ICSSCFG + 0x30U)
#define ICSSCFG_SPP             (ICSSCFG + 0x34U)
#define ICSSCFG_CORE_SYNC       (ICSSCFG + 0x3CU)
#define IEP0                    (0x2E000U)
#define IEP1                    (0x2F000U)
#define IEP_GLOBAL_CFG          (0x00U)
#define IEP_COUNT0              (0x10U)
#define IEP_COUNT1              (0x14U)
#define IEP_CMP_CFG             (0x70U)
#define IEP_CMP0_0              (0x78U)
#define IEP_CMP0_1              (0x7CU)
#define IEP_DFLT_CYCLE_NS       (1000000U)

#define MII_RT                  (0x32000U)
#define MII_RT_TXCFG0           (MII_RT + 0x10U)
#define MII_RT_TXCFG1           (MII_RT + 0x14U)
#define MII_RT_TX_IPG(s)        (MII_RT + 0x30U + ((s) * 4U))
#define MII_RT_RX_FRMS0         (MII_RT + 0x40U)
#define MII_RT_RX_FRMS1         (MII_RT + 0x44U)
#define MII_RT_RX_PCNT0         (MII_RT + 0x48U)
#define MII_RT_RX_PCNT1         (MII_RT + 0x4CU)
#define MDIO_BASE_OFS           (0x32400U)

#define MII_G                   (0x33000U)
#define MII_G_ICSS_G_CFG        (MII_G + 0x000U)
#define MII_G_RGMII_CFG         (MII_G + 0x004U)
#define MII_G_MAC_PRU(s)        (MII_G + 0x008U + ((s) * 8U))
#define MII_G_MAC_INTERFACE     (MII_G + 0x018U)
#define MII_G_FDB_GEN_CFG2      (MII_G + 0x064U)
#define MII_G_FT3_START(s)      (MII_G + ((s) ? 0x6D4U : 0x108U))
#define MII_G_FT3_P0(s)         (MII_G + ((s) ? 0x8D4U : 0x308U))
#define MII_G_CLASS0_AND_EN(s)  (MII_G + ((s) ? 0x9D8U : 0x40CU))
#define MII_G_CLASS_CFG1_OFS    (0x48CU - 0x40CU)
#define MII_G_TX_STAT_MAX0      (MII_G + 0x600U)
#define MII_G_TX_STAT_MAX1      (MII_G + 0xBCCU)
#define MII_G_QUEUE(q)          (MII_G + 0xD00U + ((q) * 4U))
#define MII_G_QUEUE_RESET       (MII_G + 0xF40U)

#define RGMII_GIG_IN(s)         (1U << (17U + ((s) * 4U)))
#define RGMII_INBAND(s)         (1U << (16U + ((s) * 4U)))
#define RGMII_FULLDUPLEX_IN(s)  (1U << (18U + ((s) * 4U)))

/* Firmware DRAM map (fw_mem_map.h) */
#define FW_FLOW_ID_BASE         (0x0024U)
#define FW_SPL_PKT_PRIO         (0x0028U)
#define FW_QUEUE_NUM_UNTAGGED   (0x002AU)
#define FW_PRIO_REGEN           (0x002CU)
#define FW_LINK_SPEED           (0x00A8U)
#define FW_SCRATCH_ADDR         (0x00ACU)
#define FW_R30_CMD              (0x05ACU)
#define FW_BUF_POOL0            (0x05BCU)
#define FW_HOST_RX_Q_PRE        (0x0684U)
#define FW_HD_RAND_SEED         (0x0934U)
#define FW_HOST_RX_Q_EXP        (0x0940U)
#define FW_HOST_POOL_IDX        (8U)    /* host pools follow 8 port pools */
#define FW_SPEED_1G             (0x00U)
#define FW_SPEED_100M           (0x01U)
#define FW_SPEED_10M            (0x02U)
#define FW_SPEED_HD             (0x80U)
#define R30_NONE                (0xFFFF0000U)

/* CPPI5 host descriptor */
#define DESC_WORD0              ((1U << 30) | (1U << 29) | (4U << 22))
#define DESC_PKTLEN_MASK        (0x3FFFFFU)
#define DESC_PKTERR(x)          (((x) >> 28) & 0xFU)

/* ------------------------------------------------------------------------ */
/* Helpers (same shape as cpsw_min.c; each driver stands alone)             */
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

static int32_t ringInit(IcssgMin_Ring *r, uint32_t num, uint64_t *mem, uint32_t cnt)
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
    req.nav_id  = NAV_PKTDMA;
    req.index   = (uint16_t)num;
    req.addr_lo = (uint32_t)(uintptr_t)mem;
    req.count   = cnt;
    req.mode    = TISCI_MSG_VALUE_RM_RING_MODE_RING;
    req.size    = TISCI_MSG_VALUE_RM_RING_SIZE_8B;
    return Sciclient_rmRingCfg(&req, &resp, SystemP_WAIT_FOREVER);
}

static void ringPush(IcssgMin_Ring *r, const void *desc)
{
    volatile uint64_t *slot = &r->mem[r->wrIdx];

    *slot = (uint64_t)(uintptr_t)desc;
    wbCache((const void *)slot, sizeof(uint64_t));
    r->wrIdx = (r->wrIdx + 1U) & (r->cnt - 1U);
    r->occ++;
    REG32(RING_FDB(r->num)) = 1U;
}

static void *ringPop(IcssgMin_Ring *r)
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
        REG32(RING_RDB(r->num)) = 0xFFU;
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
    req.tx_fetch_size     = 16U;
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
    req.valid_params        = 0x7FFFFU;
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
        treq.valid_params      = 0x7FU;
        treq.nav_id            = NAV_PKTDMA;
        treq.flow_index        = (uint16_t)flow;
        treq.rx_size_thresh0   = (uint16_t)(ICSSG_MIN_BUF_SIZE >> 5);
        treq.rx_size_thresh1   = (uint16_t)(ICSSG_MIN_BUF_SIZE >> 5);
        treq.rx_size_thresh2   = (uint16_t)(ICSSG_MIN_BUF_SIZE >> 5);
        treq.rx_fdq0_sz1_qnum  = (uint16_t)ring;
        treq.rx_fdq0_sz2_qnum  = (uint16_t)ring;
        treq.rx_fdq0_sz3_qnum  = (uint16_t)ring;
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

#define MDIO_MANUAL_IF          (0x30U)
#define MDIO_POLL               (0x34U)
#define MDIO_POLL_EN            (0x38U)
#define MDIO_PIN                (1U << 0)
#define MDIO_OE                 (1U << 1)
#define MDIO_MDCLK              (1U << 2)
#define MDIO_HALF_CYCLE_LOOPS   (200U)

static void mdioDelay(void)
{
    volatile uint32_t i;
    for (i = 0U; i < MDIO_HALF_CYCLE_LOOPS; i++)
    {
    }
}

static void mdioSet(uintptr_t mdio, uint32_t mask, uint32_t on)
{
    uint32_t v = REG32(mdio + MDIO_MANUAL_IF);
    REG32(mdio + MDIO_MANUAL_IF) = (on != 0U) ? (v | mask) : (v & ~mask);
}

static void mdioClock(uintptr_t mdio)
{
    mdioSet(mdio, MDIO_MDCLK, 0U);
    mdioDelay();
    mdioSet(mdio, MDIO_MDCLK, 1U);
    mdioDelay();
}

static void mdioSend(uintptr_t mdio, uint32_t msb, uint32_t val)
{
    uint32_t i;
    for (i = msb; i != 0U; i >>= 1)
    {
        mdioSet(mdio, MDIO_PIN, val & i);
        mdioClock(mdio);
    }
}

static void mdioFrameStart(uintptr_t mdio, uint32_t op, uint32_t phy, uint32_t reg)
{
    mdioSet(mdio, MDIO_MDCLK, 0U);
    mdioSet(mdio, MDIO_OE, 1U);
    mdioSend(mdio, 0x80000000U, 0xFFFFFFFFU);
    mdioSend(mdio, 0x8U, op);
    mdioSend(mdio, 0x10U, phy);
    mdioSend(mdio, 0x10U, reg);
}

static void mdioFrameEnd(uintptr_t mdio)
{
    mdioSet(mdio, MDIO_MDCLK, 0U);
    mdioDelay();
    mdioClock(mdio);
}

static uint16_t mdioRead(uintptr_t mdio, uint32_t phy, uint32_t reg)
{
    uint32_t i, ack;
    uint16_t val = 0U;

    mdioFrameStart(mdio, 0x6U, phy, reg);
    mdioSet(mdio, MDIO_OE, 0U);
    mdioClock(mdio);
    ack = REG32(mdio + MDIO_MANUAL_IF) & MDIO_PIN;
    mdioClock(mdio);
    for (i = 0x8000U; i != 0U; i >>= 1)
    {
        if ((REG32(mdio + MDIO_MANUAL_IF) & MDIO_PIN) != 0U)
        {
            val |= (uint16_t)i;
        }
        mdioClock(mdio);
    }
    mdioFrameEnd(mdio);
    return (ack == 0U) ? val : 0xFFFFU;
}

static void mdioWrite(uintptr_t mdio, uint32_t phy, uint32_t reg, uint16_t val)
{
    mdioFrameStart(mdio, 0x5U, phy, reg);
    mdioSend(mdio, 0x2U, 0x2U);
    mdioSend(mdio, 0x8000U, val);
    mdioSet(mdio, MDIO_OE, 0U);
    mdioFrameEnd(mdio);
}

/* ---- PHY (TI DP83869, as on the AM243x EVM ICSSG1 ports) --------------- */

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
#define DP_OP_MODE_DECODE       (0x1DFU)

static uint16_t phyReadExt(uintptr_t mdio, uint32_t phy, uint32_t reg)
{
    mdioWrite(mdio, phy, PHY_MMD_CR, 0x001FU);
    mdioWrite(mdio, phy, PHY_MMD_DR, (uint16_t)reg);
    mdioWrite(mdio, phy, PHY_MMD_CR, 0x401FU);
    return mdioRead(mdio, phy, PHY_MMD_DR);
}

static void phyWriteExt(uintptr_t mdio, uint32_t phy, uint32_t reg, uint16_t val)
{
    mdioWrite(mdio, phy, PHY_MMD_CR, 0x001FU);
    mdioWrite(mdio, phy, PHY_MMD_DR, (uint16_t)reg);
    mdioWrite(mdio, phy, PHY_MMD_CR, 0x401FU);
    mdioWrite(mdio, phy, PHY_MMD_DR, val);
}

static void phyRmwExt(uintptr_t mdio, uint32_t phy, uint32_t reg, uint16_t mask, uint16_t val)
{
    phyWriteExt(mdio, phy, reg, (uint16_t)((phyReadExt(mdio, phy, reg) & ~mask) | (val & mask)));
}

static void phyRmw(uintptr_t mdio, uint32_t phy, uint32_t reg, uint16_t mask, uint16_t val)
{
    mdioWrite(mdio, phy, reg, (uint16_t)((mdioRead(mdio, phy, reg) & ~mask) | (val & mask)));
}

static void phyConfig(uintptr_t mdio, uint32_t phy)
{
    uint32_t tries = 0U;

    mdioWrite(mdio, phy, PHY_BMCR, 0x8000U);
    while (((mdioRead(mdio, phy, PHY_BMCR) & 0x8000U) != 0U) && (tries++ < 1000U))
    {
        ClockP_usleep(100U);
    }
    phyRmwExt(mdio, phy, DP_VTMCFG, 0x000FU, 4U);
    phyRmwExt(mdio, phy, DP_DSPFFECFG, 0x03FFU, 0x0281U);
    if ((phyReadExt(mdio, phy, DP_STRAPSTS2) & 0x0400U) != 0U)
    {
        phyRmwExt(mdio, phy, DP_FLDTHRCFG, 0x0007U, 1U);
    }
    phyWriteExt(mdio, phy, DP_LOOPCR, 0xE721U);
    phyRmw(mdio, phy, PHY_CTRL, 0x4000U, 0x4000U);
    phyRmw(mdio, phy, PHY_PHYCR, 0x0060U, 0x0040U);
    phyRmw(mdio, phy, PHY_CFG3, 0x0200U, 0x0200U);
    phyRmwExt(mdio, phy, DP_OP_MODE_DECODE, 0x0020U, 0U);       /* RGMII */
    phyRmwExt(mdio, phy, DP_RGMIICTL, 0x0003U, 0U);             /* 0 = clk shift on */
    phyWriteExt(mdio, phy, DP_RGMIIDCTL, 0x0327U);              /* TX 0.75, RX 2.0 ns */
    phyRmw(mdio, phy, PHY_PHYCR, 0xC000U, 0x4000U);
    phyRmwExt(mdio, phy, DP_IOMUXCFG, 0x001FU, 0x001FU);        /* 35 ohm */
    phyRmw(mdio, phy, PHY_BMCR, 0x1200U, 0x1200U);
}

/* ---- ICSSG / firmware ---------------------------------------------------- */

static void classifiersOff(uintptr_t base, uint32_t s)
{
    uintptr_t cfg1 = base + MII_G_CLASS0_AND_EN(s) + MII_G_CLASS_CFG1_OFS;
    uint32_t c, v;

    for (c = 0U; c < 16U; c++)
    {
        uintptr_t a = base + MII_G_CLASS0_AND_EN(s) + (c * 8U);
        REG32(a) = 0U;                                  /* AND enables */
        REG32(a + 4U) = 0U;                             /* OR enables */
        v = REG32(cfg1 + 4U);
        REG32(cfg1 + 4U) = v & ~((1U << (c + 16U)) | (1U << c));
        v = REG32(cfg1);
        REG32(cfg1) = v & ~(0x3U << (c * 2U));
        v = REG32(cfg1 + 8U);
        REG32(cfg1 + 8U + (c * 4U)) = (v & ~0x60U) | 0x50U;     /* gate */
    }
}

static void fwPdInit(uintptr_t base, uint32_t smemOfs, uint32_t queue, uint32_t w0,
                     uint32_t words, uint32_t num)
{
    volatile uint32_t *pd = (volatile uint32_t *)(base + ICSS_SMEM + smemOfs);
    uint32_t i, j;

    for (i = 0U; i < num; i++)
    {
        for (j = 0U; j < words; j++)
        {
            pd[j] = 0U;
        }
        pd[0] = w0;
        REG32(base + MII_G_QUEUE(queue)) = (uint32_t)(uintptr_t)pd;
        pd += words;
    }
}

static void coresEnable(uintptr_t base, uint32_t s, uint32_t en)
{
    uintptr_t c[3] = { base + ICSS_PRU_CTRL(s), base + ICSS_RTU_CTRL(s), base + ICSS_TXPRU_CTRL(s) };
    uint32_t i;

    for (i = 0U; i < 3U; i++)
    {
        REG32(c[i]) = (en != 0U) ? (REG32(c[i]) | PRU_CTRL_ENABLE) : (REG32(c[i]) & ~PRU_CTRL_ENABLE);
    }
}

static void fwLoad(uintptr_t base, uint32_t s)
{
    uintptr_t iram[3] = { base + ICSS_PRU_IRAM(s), base + ICSS_RTU_IRAM(s), base + ICSS_TXPRU_IRAM(s) };
    const IcssgMin_Fw *fw = &gIcssgFw[s];
    uint32_t i, w;

    for (i = 0U; i < 3U; i++)
    {
        for (w = 0U; w < (fw->size[i] / 4U); w++)
        {
            REG32(iram[i] + (w * 4U)) = fw->img[i][w];
        }
    }
}

/* Firmware/MMR configuration for one slice (IcssgUtils_fwConfig, dual-MAC) */
static void fwConfig(IcssgMin_Obj *h, uint32_t flowBase)
{
    uintptr_t base = h->base;
    uintptr_t dram = h->dram;
    uint32_t s = h->slice;
    uint32_t q, start, end;
    /* {SMEM offset, queue, word0, words, count}: port LO/HI, host LO/HI, host special */
    static const uint32_t pdCfg[2][5][5] =
    {
        { { 0x2F6CU, 33U, 0x000000U, 2U, 64U }, { 0x2104U, 32U, 0x200000U, 2U, 64U },
          { 0x5F0CU, 35U, 0x000000U, 2U, 64U }, { 0x5AA4U, 34U, 0x200000U, 2U, 64U },
          { 0x7AACU, 40U, 0x400000U, 5U, 16U } },
        { { 0x4C3CU, 37U, 0x800000U, 2U, 64U }, { 0x3DD4U, 36U, 0xA00000U, 2U, 64U },
          { 0x67DCU, 39U, 0x800000U, 2U, 64U }, { 0x6374U, 38U, 0xA00000U, 2U, 64U },
          { 0x7EACU, 41U, 0xC00000U, 5U, 16U } },
    };

    REG32(base + ICSSCFG_IEPCLK) = 1U;
    ClockP_usleep(100U * 1000U);
    REG32(base + ICSSCFG_CORE_SYNC) = 0U;
    REG32(base + IEP0 + IEP_GLOBAL_CFG) = 1U | (3U << 4) | (3U << 8);
    REG32(base + IEP1 + IEP_GLOBAL_CFG) = 1U | (3U << 4) | (3U << 8);
    REG32(base + IEP0 + IEP_CMP0_0) = IEP_DFLT_CYCLE_NS - 4U;
    REG32(base + IEP0 + IEP_CMP0_1) = IEP_DFLT_CYCLE_NS - 4U;
    REG32(base + IEP0 + IEP_COUNT0) = 0U;
    REG32(base + IEP0 + IEP_COUNT1) = 0U;
    REG32(base + IEP0 + IEP_CMP_CFG) |= 0x20673U;
    REG32(base + ICSSCFG_GPCFG0) = 0x08000003U;
    REG32(base + ICSSCFG_GPCFG1) = 0x08000003U;

    REG32(base + ICSS_PRU_CTRL(s) + PRU_C28_PTR) = 0x100U;
    REG32(base + ICSS_RTU_CTRL(s) + PRU_C28_PTR) = 0x100U;
    REG32(base + ICSS_TXPRU_CTRL(s) + PRU_C28_PTR) = 0x100U;

    for (q = 0U; q < 8U; q++)
    {
        REG32(base + MII_G_QUEUE_RESET) = (s * 8U) + q;
    }
    REG32(base + MII_G_QUEUE_RESET) = 16U + s;              /* recycle queue */
    for (q = 0U; q < 5U; q++)
    {
        REG32(base + MII_G_QUEUE_RESET) = pdCfg[s][q][1];
    }
    for (q = 0U; q < 5U; q++)
    {
        fwPdInit(base, pdCfg[s][q][0], pdCfg[s][q][1], pdCfg[s][q][2], pdCfg[s][q][3], pdCfg[s][q][4]);
    }

    /* Firmware pools: scratch, one host pool, host egress queues */
    REG32(dram + FW_SCRATCH_ADDR) = (uint32_t)(uintptr_t)h->fwScratch;
    REG32(dram + FW_BUF_POOL0 + (FW_HOST_POOL_IDX * 8U)) = (uint32_t)(uintptr_t)h->fwHostPool;
    REG32(dram + FW_BUF_POOL0 + (FW_HOST_POOL_IDX * 8U) + 4U) = ICSSG_MIN_HOST_POOL_SIZE;
    start = (uint32_t)(uintptr_t)h->fwHostQueue;
    end   = start + (ICSSG_MIN_HOST_QUEUE_NUM * ICSSG_MIN_HOST_QUEUE_SIZE);
    REG32(dram + FW_HOST_RX_Q_EXP)       = start;
    REG32(dram + FW_HOST_RX_Q_EXP + 4U)  = start;
    REG32(dram + FW_HOST_RX_Q_EXP + 8U)  = start;
    REG32(dram + FW_HOST_RX_Q_EXP + 12U) = end;
    REG32(dram + FW_HOST_RX_Q_PRE)       = 0U;              /* no preemption */
    REG32(dram + FW_HOST_RX_Q_PRE + 4U)  = 0U;
    REG32(dram + FW_HOST_RX_Q_PRE + 8U)  = 0U;
    REG32(dram + FW_HOST_RX_Q_PRE + 12U) = 0U;

    REG32(dram + FW_FLOW_ID_BASE) = flowBase;
    REG8(dram + FW_SPL_PKT_PRIO) = 0U;
    REG8(dram + FW_QUEUE_NUM_UNTAGGED) = 0U;
    REG32(base + MII_G_FDB_GEN_CFG2) |= (1U << s) | (1U << 2);  /* PRUx + host FDB */
    REG32(dram + FW_HD_RAND_SEED) = 0x1234567U;

    REG32(base + MII_RT_TXCFG0) = 0x1803U;
    REG32(base + MII_RT_TXCFG1) = 0x1903U;
    REG32(base + MII_RT_TX_IPG(1U)) = 0xBU;
    REG32(base + MII_RT_TX_IPG(0U)) = 0xBU;
    REG32(base + MII_RT_RX_PCNT0) = 1U;
    REG32(base + MII_RT_RX_PCNT1) = 1U;
    REG32(base + MII_RT_RX_FRMS0) = 0x07CF003FU;
    REG32(base + MII_RT_RX_FRMS1) = 0x07CF003FU;
    REG32(base + MII_G_TX_STAT_MAX0) = 0x7D8U;
    REG32(base + MII_G_TX_STAT_MAX1) = 0x7D8U;
    REG32(base + ICSSCFG_SPP) = 0xAU;
    REG32(base + MII_G_ICSS_G_CFG) = 0x1082FU;              /* RGMII, TX PRU on */
}

/* Send an R30 command to the slice firmware and wait for completion */
static int32_t r30Cmd(IcssgMin_Obj *h, const uint32_t cmd[4])
{
    volatile uint32_t *r30 = (volatile uint32_t *)(h->dram + FW_R30_CMD);
    uint32_t i, tries = 0U, busy = 1U;

    for (i = 0U; i < 4U; i++)
    {
        r30[i] = cmd[i];
    }
    while ((busy != 0U) && (tries++ < 1000U))
    {
        busy = 0U;
        for (i = 0U; i < 4U; i++)
        {
            busy |= (r30[i] != R30_NONE) ? 1U : 0U;
        }
        ClockP_usleep(10U);
    }
    return (busy == 0U) ? SystemP_SUCCESS : SystemP_FAILURE;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

int32_t IcssgMin_open(IcssgMin_Obj *h, const IcssgMin_Cfg *cfg)
{
    const IcssgMin_SocInfo *soc = &gIcssgSoc[cfg->icssg & 1U];
    uint32_t s = cfg->slice & 1U;
    uint32_t i, macLo, macHi;
    uintptr_t base;
    int32_t status;

    if ((soc->base == 0U) || (((ICSSG_MIN_SLICES >> s) & 1U) == 0U))
    {
        return SystemP_FAILURE;
    }
    memset(&h->txRing, 0, sizeof(*h) - offsetof(IcssgMin_Obj, txRing));
    base = soc->base;
    h->base = base;
    h->dram = base + ((s != 0U) ? ICSS_DRAM1 : 0U);
    h->slice = (uint8_t)s;
    h->phyAddr = cfg->phyAddr;

    status = powerOn(soc->devId);
#if (ICSSG_MIN_CORE_CLK_HZ != 0U)
    if (status == SystemP_SUCCESS)
    {
        status = Sciclient_pmSetModuleClkFreq(soc->devId, TISCI_DEV_PRU_ICSSG1_CORE_CLK,
                                              ICSSG_MIN_CORE_CLK_HZ,
                                              TISCI_MSG_FLAG_CLOCK_ALLOW_FREQ_CHANGE,
                                              SystemP_WAIT_FOREVER);
    }
#endif

    /* DMA first, like the Enet LLD (firmware starts pushing once enabled) */
    if (status == SystemP_SUCCESS)
    {
        status  = ringInit(&h->txRing, soc->txRing[s], h->txRingMem, ICSSG_MIN_NUM_TX);
        status |= ringInit(&h->rxRing, 112U + soc->rxFlow[s], h->rxRingMem, ICSSG_MIN_NUM_RX);
        status |= txChInit(soc->txCh[s]);
        status |= rxChInit(soc->rxCh[s]);
        status |= flowInit(soc->rxFlow[s], 112U + soc->rxFlow[s]);
        status |= psilPair(soc->txCh[s] + 0x1000U, soc->psilTx[s]);
        status |= psilPair(soc->psilRx[s], soc->rxCh[s] + 0x9000U);
    }

    if (status == SystemP_SUCCESS)
    {
        for (i = 0U; i < ICSSG_MIN_NUM_TX; i++)
        {
            IcssgMin_Desc *d = &h->txDesc[i];
            d->bufPtr[0] = (uint32_t)(uintptr_t)h->txBuf[i];
            d->orgBufPtr[0] = d->bufPtr[0];
            d->orgBufLen = ICSSG_MIN_BUF_SIZE;
            h->txFree[i] = (uint8_t)i;
        }
        h->txFreeCnt = ICSSG_MIN_NUM_TX;

        for (i = 0U; i < ICSSG_MIN_NUM_RX; i++)
        {
            IcssgMin_Desc *d = &h->rxDesc[i];
            d->descInfo  = DESC_WORD0;
            d->bufPtr[0] = (uint32_t)(uintptr_t)h->rxBuf[i];
            d->bufInfo1  = ICSSG_MIN_BUF_SIZE;
            d->orgBufPtr[0] = d->bufPtr[0];
            d->orgBufLen = ICSSG_MIN_BUF_SIZE;
            wbCache(d, sizeof(*d));
            invCache(h->rxBuf[i], ICSSG_MIN_BUF_SIZE);
            ringPush(&h->rxRing, d);
        }

        REG32(CHRT_PEER8(TCHANRT_BASE, soc->txCh[s])) |= CHRT_EN;
        REG32(CHRT_CTL(TCHANRT_BASE, soc->txCh[s])) = CHRT_EN;
        REG32(CHRT_CTL(RCHANRT_BASE, soc->rxCh[s])) = CHRT_EN;
        REG32(CHRT_PEER8(RCHANRT_BASE, soc->rxCh[s])) |= CHRT_EN;

        /* Shared RAM holds both slices' descriptor pools: clear it once */
        if (gIcssgSmemInit[cfg->icssg & 1U] == 0U)
        {
            memset((void *)(base + ICSS_SMEM), 0, ICSS_SMEM_SIZE);
            gIcssgSmemInit[cfg->icssg & 1U] = 1U;
        }

        classifiersOff(base, s);
        for (i = 0U; i < 4U; i++)
        {
            REG32(h->dram + FW_R30_CMD + (i * 4U)) = R30_NONE;
        }
        /* FT3 filter 8: priority-tagged frames */
        REG32(MII_G_FT3_START(s) + base + (8U * 32U)) = 0xCU;
        REG32(MII_G_FT3_START(s) + base + (8U * 32U) + 4U) = 0U;
        REG32(MII_G_FT3_START(s) + base + (8U * 32U) + 8U) = 0U;
        REG32(MII_G_FT3_START(s) + base + (8U * 32U) + 12U) = 0U;
        REG32(MII_G_FT3_START(s) + base + (8U * 32U) + 16U) = 0U;
        REG32(MII_G_FT3_START(s) + base + (8U * 32U) + 20U) = 0x5U;
        REG32(MII_G_FT3_START(s) + base + (8U * 32U) + 24U) = 0x81U;
        REG32(MII_G_FT3_START(s) + base + (8U * 32U) + 28U) = 0x00F00000U;
        REG32(MII_G_FT3_P0(s) + base + (8U * 16U)) = 0U;
        REG32(MII_G_FT3_P0(s) + base + (8U * 16U) + 4U) = 0U;
        REG32(MII_G_FT3_P0(s) + base + (8U * 16U) + 8U) = 0xFFFFFFFFU;
        REG32(MII_G_FT3_P0(s) + base + (8U * 16U) + 12U) = 0xFFFFFFFFU;

        coresEnable(base, s, 0U);
        fwConfig(h, soc->rxFlow[s]);
        fwLoad(base, s);
        coresEnable(base, s, 1U);

        for (i = 0U; i < 8U; i++)
        {
            REG8(h->dram + FW_PRIO_REGEN + i) = 0U;     /* every PCP -> queue 0 */
        }

        macLo = (uint32_t)cfg->macAddr[0] | ((uint32_t)cfg->macAddr[1] << 8) |
                ((uint32_t)cfg->macAddr[2] << 16) | ((uint32_t)cfg->macAddr[3] << 24);
        macHi = (uint32_t)cfg->macAddr[4] | ((uint32_t)cfg->macAddr[5] << 8);
        REG32(base + MII_G_MAC_PRU(s)) = macLo;
        REG32(base + MII_G_MAC_PRU(s) + 4U) = macHi;
        REG32(base + MII_G_MAC_INTERFACE) = macLo;
        REG32(base + MII_G_MAC_INTERFACE + 4U) = macHi;

        REG32(base + MDIO_BASE_OFS + MDIO_POLL_EN) = 0U;
        REG32(base + MDIO_BASE_OFS + MDIO_POLL) |= (1U << 31) | (1U << 30);
        phyConfig(base + MDIO_BASE_OFS, h->phyAddr);
    }
    return status;
}

uint32_t IcssgMin_poll(IcssgMin_Obj *h)
{
    static const uint32_t cmdFwd[4] = { 0xFFBB0000U, 0xFCFF0000U, 0xDCF30000U, R30_NONE };
    static const uint32_t cmdDis[4] = { 0xFFFF0004U, 0xFFFF0100U, 0xFFFF0104U, R30_NONE };
    uintptr_t mdio = h->base + MDIO_BASE_OFS;
    uint32_t s = h->slice;
    uint32_t up;

    (void)mdioRead(mdio, h->phyAddr, PHY_BMSR);
    up = ((mdioRead(mdio, h->phyAddr, PHY_BMSR) & 0x0004U) != 0U) ? 1U : 0U;
    if (up != h->linkUp)
    {
        if (up != 0U)
        {
            uint16_t sts = mdioRead(mdio, h->phyAddr, PHY_PHYSTS);
            uint32_t rg = REG32(h->base + MII_G_RGMII_CFG);
            uint8_t fwSpeed;

            h->speed = (uint8_t)((sts >> 14) & 0x3U);
            h->fullDuplex = (uint8_t)((sts >> 13) & 0x1U);
            rg &= ~(RGMII_GIG_IN(s) | RGMII_INBAND(s) | RGMII_FULLDUPLEX_IN(s));
            rg |= (h->fullDuplex != 0U) ? RGMII_FULLDUPLEX_IN(s) : 0U;
            if (h->speed == ICSSG_MIN_SPEED_1000)
            {
                rg |= RGMII_GIG_IN(s);
                fwSpeed = FW_SPEED_1G;
            }
            else if (h->speed == ICSSG_MIN_SPEED_100)
            {
                fwSpeed = FW_SPEED_100M;
            }
            else
            {
                rg |= RGMII_INBAND(s);
                fwSpeed = FW_SPEED_10M;
            }
            REG32(h->base + MII_G_RGMII_CFG) = rg;
            if (h->speed != ICSSG_MIN_SPEED_10)
            {
                REG32(h->base + MII_RT_TX_IPG(s)) = (h->speed == ICSSG_MIN_SPEED_1000) ? 0x0BU : 0x17U;
                if (s != 0U)
                {
                    /* erratum: RMW TX_IPG0 so TX_IPG1 gets latched */
                    REG32(h->base + MII_RT_TX_IPG(0U)) = REG32(h->base + MII_RT_TX_IPG(0U));
                }
            }
            if ((h->fullDuplex == 0U) && (fwSpeed != FW_SPEED_1G))
            {
                fwSpeed |= FW_SPEED_HD;
            }
            REG8(h->dram + FW_LINK_SPEED) = fwSpeed;
            (void)r30Cmd(h, cmdFwd);
        }
        else
        {
            (void)r30Cmd(h, cmdDis);
        }
        h->linkUp = (uint8_t)up;
    }
    return h->linkUp;
}

uint8_t *IcssgMin_getTxBuf(IcssgMin_Obj *h)
{
    IcssgMin_Desc *d;

    while ((d = (IcssgMin_Desc *)ringPop(&h->txRing)) != NULL)
    {
        h->txFree[h->txFreeCnt++] = (uint8_t)(d - h->txDesc);
    }
    return (h->txFreeCnt != 0U) ? h->txBuf[h->txFree[--h->txFreeCnt]] : NULL;
}

int32_t IcssgMin_send(IcssgMin_Obj *h, uint8_t *buf, uint32_t len)
{
    uint32_t idx = (uint32_t)(buf - h->txBuf[0]) / ICSSG_MIN_BUF_SIZE;
    IcssgMin_Desc *d;

    if ((idx >= ICSSG_MIN_NUM_TX) || (len > ICSSG_MIN_BUF_SIZE))
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
    d->srcDstTag = 0U;
    d->epib[0] = 0U; d->epib[1] = 0U;           /* no TX timestamp / offloads */
    d->bufInfo1  = len;
    wbCache(buf, len);
    wbCache(d, sizeof(*d));
    ringPush(&h->txRing, d);
    h->txPkts++;
    return SystemP_SUCCESS;
}

uint8_t *IcssgMin_recv(IcssgMin_Obj *h, uint32_t *len)
{
    IcssgMin_Desc *d;
    uint8_t *buf = NULL;

    while ((buf == NULL) && ((d = (IcssgMin_Desc *)ringPop(&h->rxRing)) != NULL))
    {
        uint32_t idx = (uint32_t)(d - h->rxDesc);
        uint32_t n;

        invCache(d, sizeof(*d));
        n = d->descInfo & DESC_PKTLEN_MASK;
        if ((DESC_PKTERR(d->pktInfo1) != 0U) || (n < 4U))
        {
            h->rxErrs++;
            IcssgMin_recvDone(h, h->rxBuf[idx]);
        }
        else
        {
            buf = h->rxBuf[idx];
            *len = n - 4U;                      /* firmware keeps the FCS */
            invCache(buf, n);
            h->rxPkts++;
        }
    }
    return buf;
}

void IcssgMin_recvDone(IcssgMin_Obj *h, uint8_t *buf)
{
    uint32_t idx = (uint32_t)(buf - h->rxBuf[0]) / ICSSG_MIN_BUF_SIZE;
    IcssgMin_Desc *d = &h->rxDesc[idx];

    d->descInfo = DESC_WORD0;
    d->bufInfo1 = ICSSG_MIN_BUF_SIZE;
    wbCache(d, sizeof(*d));
    invCache(buf, ICSSG_MIN_BUF_SIZE);
    ringPush(&h->rxRing, d);
}
