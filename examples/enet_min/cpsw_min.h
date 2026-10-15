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
 * cpsw_min: minimal bare-metal CPSW3G (AM64x/AM243x) Ethernet driver.
 *
 *  - One PKTDMA TX channel + one RX channel/flow, host descriptors, polled.
 *  - ALE in bypass mode; TX is directed to a MAC port through the
 *    descriptor dstTag, RX reports the ingress MAC port.
 *  - PHY bring-up (TI DP83867) over MDIO in manual (bit-bang) mode, which
 *    is also the workaround for silicon erratum i2329.
 *  - Zero-copy buffer API; no RTOS, no Enet LLD, no UDMA LLD.
 *
 * Resources (channels, rings, flows) are configured through the SDK
 * Sciclient which System_init() already links, so they must be assigned to
 * this core in the RM board configuration (default for r5fss0-0).
 *
 * Adding a MAC port = one more entry in CpswMin_Cfg.port[] (MAX_PORTS=2).
 */

#ifndef CPSW_MIN_H_
#define CPSW_MIN_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CPSW_MIN_NUM_TX
#define CPSW_MIN_NUM_TX         (8U)    /* TX descriptors/buffers, power of 2 */
#endif
#ifndef CPSW_MIN_NUM_RX
#define CPSW_MIN_NUM_RX         (8U)    /* RX descriptors/buffers, power of 2 */
#endif
#define CPSW_MIN_BUF_SIZE       (1536U) /* bytes per packet buffer */
#define CPSW_MIN_DESC_SIZE      (128U)  /* host descriptor size/alignment */
#define CPSW_MIN_MAX_PORTS      (2U)
#define CPSW_MIN_MTU            (1522U) /* max frame incl. VLAN tag, no FCS */

/* Link speed reported by CpswMin_poll() */
#define CPSW_MIN_SPEED_10       (0U)
#define CPSW_MIN_SPEED_100      (1U)
#define CPSW_MIN_SPEED_1000     (2U)

typedef struct
{
    uint8_t  macPort;       /* 1 or 2 */
    uint8_t  phyAddr;       /* MDIO address of the PHY */
} CpswMin_PortCfg;

typedef struct
{
    uint8_t         macAddr[6];
    uint32_t        numPorts;
    CpswMin_PortCfg port[CPSW_MIN_MAX_PORTS];
} CpswMin_Cfg;

typedef struct
{
    uint8_t  macPort;
    uint8_t  phyAddr;
    uint8_t  linkUp;
    uint8_t  speed;
    uint8_t  fullDuplex;
} CpswMin_Port;

/* Host packet descriptor (CPPI5 HMPD + 16 B EPIB + 16 B PS words) */
typedef struct
{
    volatile uint32_t descInfo;
    volatile uint32_t pktInfo1;
    volatile uint32_t pktInfo2;
    volatile uint32_t srcDstTag;
    volatile uint32_t nextDesc[2];
    volatile uint32_t bufPtr[2];
    volatile uint32_t bufInfo1;
    volatile uint32_t orgBufLen;
    volatile uint32_t orgBufPtr[2];
    volatile uint32_t epib[4];
    volatile uint32_t psData[4];
    uint8_t           pad[CPSW_MIN_DESC_SIZE - 80U];
} CpswMin_Desc;

/* PKTDMA dual ring (host writes forward, DMA returns in reverse) */
typedef struct
{
    volatile uint64_t *mem;
    uint32_t           num;     /* ring number */
    uint32_t           cnt;     /* element count */
    uint32_t           wrIdx;
    uint32_t           rdIdx;
    uint32_t           occ;     /* elements owned by hardware */
} CpswMin_Ring;

/*
 * Driver object. Keep it in normal (cached) MSRAM .bss; all DMA memory is
 * embedded so one static object is everything the driver needs.
 */
typedef struct
{
    CpswMin_Desc  txDesc[CPSW_MIN_NUM_TX] __attribute__((aligned(CPSW_MIN_DESC_SIZE)));
    CpswMin_Desc  rxDesc[CPSW_MIN_NUM_RX] __attribute__((aligned(CPSW_MIN_DESC_SIZE)));
    uint8_t       txBuf[CPSW_MIN_NUM_TX][CPSW_MIN_BUF_SIZE] __attribute__((aligned(128)));
    uint8_t       rxBuf[CPSW_MIN_NUM_RX][CPSW_MIN_BUF_SIZE] __attribute__((aligned(128)));
    uint64_t      txRingMem[CPSW_MIN_NUM_TX] __attribute__((aligned(128)));
    uint64_t      rxRingMem[CPSW_MIN_NUM_RX] __attribute__((aligned(128)));
    CpswMin_Ring  txRing;
    CpswMin_Ring  rxRing;
    uint8_t       txFree[CPSW_MIN_NUM_TX];  /* stack of free TX indices */
    uint32_t      txFreeCnt;
    uint32_t      numPorts;
    CpswMin_Port  port[CPSW_MIN_MAX_PORTS];
    uint32_t      txPkts;
    uint32_t      rxPkts;
    uint32_t      rxErrs;
} CpswMin_Obj;

/* Power up CPSW, configure DMA/ALE/host port/MACs and start PHY autoneg. */
int32_t  CpswMin_open(CpswMin_Obj *h, const CpswMin_Cfg *cfg);

/* Poll PHYs; programs a MAC port on link change. Returns link-up bitmask
 * indexed by port[] position. Call periodically (e.g. every 100 ms). */
uint32_t CpswMin_poll(CpswMin_Obj *h);

/* Get a free TX buffer (reclaims completed TX first); NULL if none. */
uint8_t *CpswMin_getTxBuf(CpswMin_Obj *h);

/* Send len bytes from a buffer returned by CpswMin_getTxBuf() on macPort. */
int32_t  CpswMin_send(CpswMin_Obj *h, uint8_t *buf, uint32_t len, uint32_t macPort);

/* Next received frame or NULL. Buffer stays owned by the caller until
 * CpswMin_recvDone(). *macPort is the ingress MAC port. */
uint8_t *CpswMin_recv(CpswMin_Obj *h, uint32_t *len, uint32_t *macPort);

/* Return an RX buffer to hardware (any order). */
void     CpswMin_recvDone(CpswMin_Obj *h, uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* CPSW_MIN_H_ */
