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
 * icssg_min: minimal bare-metal ICSSG dual-MAC Ethernet driver (AM64x/AM243x).
 *
 *  - One object = one ICSSG slice = one MAC port running the TI dual-MAC
 *    firmware. Open a second object on the other slice for a second port.
 *  - One PKTDMA TX channel + one RX channel/flow, host descriptors, polled.
 *  - Single host queue (QoS 1, classifiers off, PCP regenerated to 0), so
 *    the firmware only needs one 8 KB host buffer pool per port.
 *  - PHY bring-up (TI DP83869) over the ICSSG MDIO in manual mode.
 *  - No PRUICSS driver, no Enet LLD, no UDMA LLD, no RTOS.
 *
 * ICSSG0 needs only another row in the resource table in icssg_min.c.
 */

#ifndef ICSSG_MIN_H_
#define ICSSG_MIN_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ICSSG_MIN_NUM_TX
#define ICSSG_MIN_NUM_TX        (8U)    /* power of 2 */
#endif
#ifndef ICSSG_MIN_NUM_RX
#define ICSSG_MIN_NUM_RX        (8U)    /* power of 2 */
#endif
/* Bit mask of slices whose firmware is linked in (bit0 = slice 0). Each
 * slice's firmware is ~14 KB of .rodata. */
#ifndef ICSSG_MIN_SLICES
#define ICSSG_MIN_SLICES        (0x3U)
#endif
#define ICSSG_MIN_BUF_SIZE      (1536U)
#define ICSSG_MIN_DESC_SIZE     (128U)

/* Firmware memory per port (gigabit dual-MAC sizes from the Enet LLD) */
#define ICSSG_MIN_HOST_POOL_SIZE    (8U * 1024U)
#define ICSSG_MIN_HOST_QUEUE_SIZE   (8U * 1024U)
#define ICSSG_MIN_HOST_QUEUE_NUM    (2U)
#define ICSSG_MIN_HOST_QUEUE_PAD    (2048U)
#define ICSSG_MIN_SCRATCH_SIZE      (2048U)

#define ICSSG_MIN_SPEED_10      (0U)
#define ICSSG_MIN_SPEED_100     (1U)
#define ICSSG_MIN_SPEED_1000    (2U)

typedef struct
{
    uint8_t  icssg;         /* ICSSG instance: 1 (PRU_ICSSG1) */
    uint8_t  slice;         /* 0 = MAC port 1, 1 = MAC port 2 */
    uint8_t  phyAddr;
    uint8_t  macAddr[6];
} IcssgMin_Cfg;

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
    uint8_t           pad[ICSSG_MIN_DESC_SIZE - 80U];
} IcssgMin_Desc;

typedef struct
{
    volatile uint64_t *mem;
    uint32_t           num;
    uint32_t           cnt;
    uint32_t           wrIdx;
    uint32_t           rdIdx;
    uint32_t           occ;
} IcssgMin_Ring;

typedef struct
{
    /* Memory owned by the firmware (host pool, host egress queue, scratch) */
    uint8_t        fwHostPool[ICSSG_MIN_HOST_POOL_SIZE] __attribute__((aligned(128)));
    uint8_t        fwHostQueue[(ICSSG_MIN_HOST_QUEUE_NUM * ICSSG_MIN_HOST_QUEUE_SIZE) +
                               ICSSG_MIN_HOST_QUEUE_PAD] __attribute__((aligned(128)));
    uint8_t        fwScratch[ICSSG_MIN_SCRATCH_SIZE] __attribute__((aligned(128)));
    /* Memory owned by the host DMA */
    IcssgMin_Desc  txDesc[ICSSG_MIN_NUM_TX] __attribute__((aligned(ICSSG_MIN_DESC_SIZE)));
    IcssgMin_Desc  rxDesc[ICSSG_MIN_NUM_RX] __attribute__((aligned(ICSSG_MIN_DESC_SIZE)));
    uint8_t        txBuf[ICSSG_MIN_NUM_TX][ICSSG_MIN_BUF_SIZE] __attribute__((aligned(128)));
    uint8_t        rxBuf[ICSSG_MIN_NUM_RX][ICSSG_MIN_BUF_SIZE] __attribute__((aligned(128)));
    uint64_t       txRingMem[ICSSG_MIN_NUM_TX] __attribute__((aligned(128)));
    uint64_t       rxRingMem[ICSSG_MIN_NUM_RX] __attribute__((aligned(128)));
    IcssgMin_Ring  txRing;
    IcssgMin_Ring  rxRing;
    uint8_t        txFree[ICSSG_MIN_NUM_TX];
    uint32_t       txFreeCnt;
    uintptr_t      base;        /* ICSSG base (DRAM0) */
    uintptr_t      dram;        /* this slice's DRAM */
    uint8_t        slice;
    uint8_t        phyAddr;
    uint8_t        linkUp;
    uint8_t        speed;
    uint8_t        fullDuplex;
    uint32_t       txPkts;
    uint32_t       rxPkts;
    uint32_t       rxErrs;
} IcssgMin_Obj;

/* Power up the ICSSG, load and start the dual-MAC firmware on cfg->slice,
 * configure DMA and start PHY autoneg. */
int32_t  IcssgMin_open(IcssgMin_Obj *h, const IcssgMin_Cfg *cfg);

/* Poll the PHY; on link change programs the RGMII speed and tells the
 * firmware. Returns 1 when the link is up. */
uint32_t IcssgMin_poll(IcssgMin_Obj *h);

uint8_t *IcssgMin_getTxBuf(IcssgMin_Obj *h);
int32_t  IcssgMin_send(IcssgMin_Obj *h, uint8_t *buf, uint32_t len);
uint8_t *IcssgMin_recv(IcssgMin_Obj *h, uint32_t *len);
void     IcssgMin_recvDone(IcssgMin_Obj *h, uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* ICSSG_MIN_H_ */
