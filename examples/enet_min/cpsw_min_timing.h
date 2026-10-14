/*
 * cpsw_min_timing.h: bench-only latency split for the cpsw_min echo example
 * (build with -DCPSW_MIN_TIMING). Not part of the driver.
 *
 * The CPSW CPTS timestamps every frame of the echo test at the MAC: RX at the
 * SFD of the incoming frame, TX at the SFD of the echo. The test ethertype
 * (0x88B5) is programmed as the port's Annex F LTYPE, unicast timestamping is
 * enabled, and the sequence ID is taken from payload offset 4, where the SOEM
 * echo tester keeps its sequence number. TX minus RX is the time the frame
 * spent in this board. The R5F cycle counter splits the software part of it
 * into recv (descriptor + cache invalidate), copy, send (cache writeback +
 * ring push) and recvDone (buffer handback). Whatever remains is the incoming
 * frame's serialisation, RX DMA, the poll detecting it, and TX DMA up to the
 * SFD.
 *
 * Results live in gTiming and are read over JTAG after the run.
 */
#ifndef CPSW_MIN_TIMING_H_
#define CPSW_MIN_TIMING_H_

#include <stdint.h>
#include <kernel/dpl/ClockP.h>
#include <kernel/dpl/CycleCounterP.h>
#include <drivers/hw_include/cslr_soc.h>

#define TM_REG32(a)             (*(volatile uint32_t *)(uintptr_t)(a))
#define TM_CPSW                 (CSL_CPSW0_NUSS_BASE)
#define TM_PN(p)                (TM_CPSW + 0x22000U + (((p) - 1U) * 0x1000U))
#define TM_PN_TS_CTL(p)         (TM_PN(p) + 0x310U)
#define TM_PN_TS_SEQ_LTYPE(p)   (TM_PN(p) + 0x314U)
#define TM_PN_TS_CTL_LTYPE2(p)  (TM_PN(p) + 0x31CU)
#define TM_CPTS_CONTROL         (TM_CPSW + 0x3D004U)
#define TM_CPTS_TS_PUSH         (TM_CPSW + 0x3D00CU)
#define TM_CPTS_INTSTAT_RAW     (TM_CPSW + 0x3D020U)
#define TM_CPTS_EVENT_POP       (TM_CPSW + 0x3D030U)
#define TM_CPTS_EVENT_0         (TM_CPSW + 0x3D034U)
#define TM_CPTS_EVENT_1         (TM_CPSW + 0x3D038U)
#define TM_CPTS_EVENT_3         (TM_CPSW + 0x3D040U)

#define TM_RX_DCNT              (CSL_DMASS0_PKTDMA_RCHANRT_BASE + 0x404U + (16U * 0x1000U))
#define TM_RING_FOCC(r)         (CSL_DMASS0_PKTDMA_RINGRT_BASE + 0x0018U + ((r) * 0x2000U))
#define TM_RING_ROCC(r)         (CSL_DMASS0_PKTDMA_RINGRT_BASE + 0x1018U + ((r) * 0x2000U))
#define TM_RX_RING              (129U)
#define TM_DROPS                (64U)

#define TM_EV_PUSH              (0U)
#define TM_EV_ETH_RX            (4U)
#define TM_EV_ETH_TX            (5U)

#define TM_NUM_SIZES            (6U)
#define TM_SLOTS                (256U)

typedef struct
{
    uint32_t len;
    uint32_t n;             /* echoes with both CPTS stamps, window-1 only */
    uint32_t resMin;        /* CPTS ticks, TX SFD - RX SFD */
    uint32_t resMax;
    uint64_t resSum;
    uint64_t recvSum;       /* CPU cycles */
    uint64_t copySum;
    uint64_t sendSum;
    uint64_t doneSum;
} TmSize;

typedef struct
{
    uint32_t cpuHz;         /* measured against ClockP */
    uint32_t cptsHz;        /* measured against ClockP */
    uint32_t events;
    uint32_t unpaired;
    uint32_t idleN;         /* empty poll iterations */
    uint32_t idleMin;
    uint32_t idleMax;
    uint64_t idleSum;
    TmSize   sz[TM_NUM_SIZES];
} TmStats;

typedef struct
{
    uint64_t rxTs;
    uint64_t txTs;
    uint16_t seq;
    uint8_t  haveRx;
    uint8_t  haveTx;
    uint32_t len;
    uint32_t recv, copy, send, done;
    uint64_t gap;           /* CPTS ticks since the previous RX SFD */
} TmSlot;

typedef struct
{
    uint32_t cycle;         /* CPU cycle when the drop was seen */
    uint32_t delta;         /* drops since the previous check */
    uint32_t focc;          /* RX ring forward (free) occupancy */
    uint32_t rocc;          /* RX ring reverse (completion) occupancy */
    uint32_t sinceLoop;     /* cycles since the previous loop iteration */
    uint32_t lastBatch;     /* frames handled in the previous batch */
} TmDrop;

volatile TmDrop gTmDrop[TM_DROPS];
volatile uint32_t gTmDropN;
static uint32_t gTmDcnt, gTmLoopCyc, gTmBatch;

/* PKTDMA RX drops: log the instant and the ring state of each new drop */
static void tmDropCheck(uint32_t nowCyc, uint32_t batch)
{
    uint32_t d = TM_REG32(TM_RX_DCNT);

    if ((d != gTmDcnt) && (gTmDcnt != 0U))
    {
        uint32_t i = gTmDropN % TM_DROPS;

        gTmDrop[i].cycle = nowCyc;
        gTmDrop[i].delta = d - gTmDcnt;
        gTmDrop[i].focc = TM_REG32(TM_RING_FOCC(TM_RX_RING));
        gTmDrop[i].rocc = TM_REG32(TM_RING_ROCC(TM_RX_RING));
        gTmDrop[i].sinceLoop = nowCyc - gTmLoopCyc;
        gTmDrop[i].lastBatch = gTmBatch;
        gTmDropN++;
    }
    gTmDcnt = (d == 0U) ? 1U : d;
    gTmLoopCyc = nowCyc;
    gTmBatch = batch;
}

volatile TmStats gTiming;
static TmSlot gTmSlot[TM_SLOTS];
static uint64_t gTmLastRx;

static const uint32_t gTmLens[TM_NUM_SIZES] = { 60U, 128U, 256U, 512U, 1024U, 1514U };

static uint64_t tmPopTs(uint32_t *type, uint32_t *seq)
{
    uint32_t e0 = TM_REG32(TM_CPTS_EVENT_0);
    uint32_t e1 = TM_REG32(TM_CPTS_EVENT_1);
    uint32_t e3 = TM_REG32(TM_CPTS_EVENT_3);

    TM_REG32(TM_CPTS_EVENT_POP) = 1U;
    *type = (e1 >> 20) & 0xFU;
    *seq = e1 & 0xFFFFU;
    return ((uint64_t)e3 << 32) | e0;
}

/* CPTS time now, through a software push event (call before traffic only) */
static uint64_t tmCptsNow(void)
{
    uint32_t type = 0xFU, seq;
    uint64_t ts = 0U;

    while ((TM_REG32(TM_CPTS_INTSTAT_RAW) & 1U) != 0U)
    {
        (void)tmPopTs(&type, &seq);
    }
    TM_REG32(TM_CPTS_TS_PUSH) = 1U;
    do
    {
        while ((TM_REG32(TM_CPTS_INTSTAT_RAW) & 1U) == 0U)
        {
        }
        ts = tmPopTs(&type, &seq);
    } while (type != TM_EV_PUSH);
    return ts;
}

static void tmInit(uint32_t macPort)
{
    uint64_t t0, c0, t1, c1;
    uint32_t c32a, c32b, i;

    for (i = 0U; i < TM_NUM_SIZES; i++)
    {
        gTiming.sz[i].len = gTmLens[i];
        gTiming.sz[i].resMin = 0xFFFFFFFFU;
    }
    gTiming.idleMin = 0xFFFFFFFFU;

    /* CPTS on, 64-bit mode; port: Annex F on LTYPE 0x88B5, any message
     * type, unicast allowed, sequence ID at payload offset 4 */
    TM_REG32(TM_CPTS_CONTROL) = (1U << 0) | (1U << 5);
    TM_REG32(TM_PN_TS_SEQ_LTYPE(macPort)) = (4U << 16) | 0x88B5U;
    TM_REG32(TM_PN_TS_CTL_LTYPE2(macPort)) |= (1U << 24);
    TM_REG32(TM_PN_TS_CTL(macPort)) = (0xFFFFU << 16) | (1U << 4) | (1U << 0);

    /* rates against the ClockP timebase, over 200 ms */
    CycleCounterP_reset();
    t0 = tmCptsNow();
    c32a = CycleCounterP_getCount32();
    c0 = ClockP_getTimeUsec();
    while ((ClockP_getTimeUsec() - c0) < 200000U)
    {
    }
    t1 = tmCptsNow();
    c32b = CycleCounterP_getCount32();
    c1 = ClockP_getTimeUsec();
    gTiming.cptsHz = (uint32_t)(((t1 - t0) * 1000000U) / (c1 - c0));
    gTiming.cpuHz  = (uint32_t)(((uint64_t)(c32b - c32a) * 1000000U) / (c1 - c0));
}

static void tmAccount(TmSlot *s)
{
    uint32_t i, res;
    /* window-1 traffic only: nothing else arrived within two frame times
     * plus 10 us of this frame, so no queueing inflates the residence */
    uint64_t quiet = ((uint64_t)(2U * (s->len + 12U) * 80U) + 10000U) *
                     (gTiming.cptsHz / 1000000U) / 1000U;

    for (i = 0U; i < TM_NUM_SIZES; i++)
    {
        if ((gTmLens[i] == s->len) && (s->gap > quiet))
        {
            volatile TmSize *z = &gTiming.sz[i];

            res = (uint32_t)(s->txTs - s->rxTs);
            z->n++;
            z->resSum += res;
            z->resMin = (res < z->resMin) ? res : z->resMin;
            z->resMax = (res > z->resMax) ? res : z->resMax;
            z->recvSum += s->recv;
            z->copySum += s->copy;
            z->sendSum += s->send;
            z->doneSum += s->done;
        }
    }
    s->haveRx = 0U;
    s->haveTx = 0U;
    s->len = 0U;
}

/* drain CPTS events; pair RX and TX stamps by sequence ID */
static void tmDrain(void)
{
    uint32_t type, seq;
    uint64_t ts;
    TmSlot *s;

    while ((TM_REG32(TM_CPTS_INTSTAT_RAW) & 1U) != 0U)
    {
        ts = tmPopTs(&type, &seq);
        gTiming.events++;
        if ((type != TM_EV_ETH_RX) && (type != TM_EV_ETH_TX))
        {
            continue;
        }
        s = &gTmSlot[(seq >> 8) % TM_SLOTS];   /* low byte of the LE seq */
        if (type == TM_EV_ETH_RX)
        {
            if (s->haveRx != 0U)
            {
                gTiming.unpaired++;
            }
            s->gap = ts - gTmLastRx;
            gTmLastRx = ts;
            s->rxTs = ts;
            s->seq = (uint16_t)seq;
            s->haveRx = 1U;
            s->haveTx = 0U;
        }
        else
        {
            s->txTs = ts;
            s->haveTx = 1U;
        }
        if ((s->haveRx != 0U) && (s->haveTx != 0U) && (s->len != 0U))
        {
            tmAccount(s);
        }
    }
}

/* software split for one echoed frame (sequence ID as the CPTS sees it) */
static uint32_t tmSeq(const uint8_t *rx)
{
    return ((uint32_t)rx[18] << 8) | rx[19];
}

static void tmSoftware(uint32_t seq, uint32_t len, uint32_t recv,
                       uint32_t copy, uint32_t send, uint32_t done)
{
    TmSlot *s = &gTmSlot[(seq >> 8) % TM_SLOTS];

    s->len = len;
    s->recv = recv;
    s->copy = copy;
    s->send = send;
    s->done = done;
    if ((s->haveRx != 0U) && (s->haveTx != 0U))
    {
        tmAccount(s);
    }
}

static void tmIdle(uint32_t cycles)
{
    gTiming.idleN++;
    gTiming.idleSum += cycles;
    gTiming.idleMin = (cycles < gTiming.idleMin) ? cycles : gTiming.idleMin;
    gTiming.idleMax = (cycles > gTiming.idleMax) ? cycles : gTiming.idleMax;
}

#endif /* CPSW_MIN_TIMING_H_ */
