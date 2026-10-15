/*
 *  Copyright (c) Texas Instruments Incorporated 2026
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

/*!
 * \file  enet_layer2_icssg_cpsw.c
 *
 * \brief Layer 2 example running ICSSG and CPSW from the same R5F core.
 *
 * Three Ethernet ports are opened by one core:
 *  - ICSSG1 dual-MAC port 1 (Enet instance CONFIG_ENET_ICSS0)
 *  - ICSSG1 dual-MAC port 2 (Enet instance CONFIG_ENET_ICSS1)
 *  - CPSW MAC port 1        (Enet instance CONFIG_ENET_CPSW0)
 *
 * Every packet received on a port is sent back on the same port, with the
 * destination MAC address set to the source MAC address of the received
 * packet and the source MAC address set to the port's own MAC address.
 */

/* ========================================================================== */
/*                             Include Files                                  */
/* ========================================================================== */

#include <stdint.h>
#include <string.h>
#include <include/core/enet_osal.h>
#include <kernel/dpl/TaskP.h>
#include <kernel/dpl/ClockP.h>
#include <kernel/dpl/SemaphoreP.h>
#include <kernel/dpl/DebugP.h>
#include <enet.h>
#include <enet_cfg.h>
#include <include/core/enet_dma.h>
#include <include/per/icssg.h>
#include <include/per/cpsw.h>
#include <enet_apputils.h>
#include <enet_appmemutils.h>
#include <enet_appmemutils_cfg.h>
#include <enet_appboardutils.h>
/* SDK includes */
#include "ti_board_config.h"
#include "ti_drivers_open_close.h"
#include "ti_board_open_close.h"
#include "ti_enet_open_close.h"
#include "ti_enet_config.h"

/* ========================================================================== */
/*                           Macros & Typedefs                                */
/* ========================================================================== */

/* Number of Ethernet ports (Enet instances) used by the example */
#define ENETAPP_PER_MAX                          (3U)

/* Max number of MAC ports per Enet instance */
#define ENETAPP_MACPORT_MAX                      (2U)

/* Max number of TX packets per Enet instance */
#define ENETAPP_TX_PKT_MAX                       (32U)

/* RX task stack size */
#define ENETAPP_TASK_STACK_SZ                    (10U * 1024U)

/* RX task priority */
#define ENETAPP_RX_TASK_PRI                      (2U)

/* ========================================================================== */
/*                         Structure Declarations                             */
/* ========================================================================== */

/* Static description of one Ethernet port used by the example */
typedef struct EnetApp_PortInfo_s
{
    /* Name used in logs */
    const char *name;

    /* Enet instance index (CONFIG_ENET_xxx from SysConfig) */
    uint32_t enetInstIdx;

    /* TX DMA channel index (ENET_DMA_TX_xxx from SysConfig) */
    uint32_t txChId;

    /* Number of TX packets of the TX DMA channel */
    uint32_t txPktNum;

    /* RX DMA channel index (ENET_DMA_RX_xxx from SysConfig) */
    uint32_t rxChId;

    /* Number of RX packets of the RX DMA channel */
    uint32_t rxPktNum;
} EnetApp_PortInfo;

/* Context of one Enet instance */
typedef struct EnetApp_PerCtxt_s
{
    /* Static port description */
    const EnetApp_PortInfo *info;

    /* Peripheral type and instance */
    Enet_Type enetType;
    uint32_t instId;

    /* MAC ports of this Enet instance */
    Enet_MacPort macPort[ENETAPP_MACPORT_MAX];
    uint8_t macPortNum;

    /* Enet and UDMA handles */
    EnetApp_HandleInfo handleInfo;

    /* Core attach info */
    EnetPer_AttachCoreOutArgs attachInfo;

    /* MAC address of the port */
    uint8_t macAddr[ENET_MAC_ADDR_LEN];

    /* DMA channels */
    EnetDma_TxChHandle hTxCh;
    EnetDma_RxChHandle hRxCh;

    /* Free TX packets of this port */
    EnetDma_PktQ txFreePktInfoQ;

    /* RX task: echoes the received packets */
    TaskP_Object rxTaskObj;

    /* Posted from the RX DMA callback */
    SemaphoreP_Object rxSemObj;

    /* Posted by the RX task when it exits */
    SemaphoreP_Object rxDoneSemObj;

    /* Posted from the event callback on asynchronous IOCTL completion (ICSSG) */
    SemaphoreP_Object asyncIoctlSemObj;

    /* Statistics */
    uint32_t rxPktCnt;
    uint32_t txPktCnt;
    uint32_t txDropCnt;
} EnetApp_PerCtxt;

typedef struct EnetApp_Obj_s
{
    /* Whether the RX tasks keep running */
    volatile bool run;

    /* This core's id */
    uint32_t coreId;

    /* One context per Enet instance */
    EnetApp_PerCtxt perCtxt[ENETAPP_PER_MAX];
} EnetApp_Obj;

/* ========================================================================== */
/*                          Function Declarations                             */
/* ========================================================================== */

void EnetApp_mainTask(void *args);

static int32_t EnetApp_open(void);

static void EnetApp_close(void);

static int32_t EnetApp_openDma(EnetApp_PerCtxt *perCtxt);

static void EnetApp_closeDma(EnetApp_PerCtxt *perCtxt);

static int32_t EnetApp_waitForLinkUp(EnetApp_PerCtxt *perCtxt);

static void EnetApp_rxTask(void *args);

static void EnetApp_printStats(void);

/* ========================================================================== */
/*                            Global Variables                                */
/* ========================================================================== */

static const EnetApp_PortInfo gEnetAppPortInfo[ENETAPP_PER_MAX] =
{
    {
        .name        = "ICSSG1 port 1",
        .enetInstIdx = CONFIG_ENET_ICSS0,
        .txChId      = ENET_DMA_TX_CH0,
        .txPktNum    = ENET_DMA_TX_CH0_NUM_PKTS,
        .rxChId      = ENET_DMA_RX_CH0,
        .rxPktNum    = ENET_DMA_RX_CH0_NUM_PKTS,
    },
    {
        .name        = "ICSSG1 port 2",
        .enetInstIdx = CONFIG_ENET_ICSS1,
        .txChId      = ENET_DMA_TX_CH1,
        .txPktNum    = ENET_DMA_TX_CH1_NUM_PKTS,
        .rxChId      = ENET_DMA_RX_CH1,
        .rxPktNum    = ENET_DMA_RX_CH1_NUM_PKTS,
    },
    {
        .name        = "CPSW port 1",
        .enetInstIdx = CONFIG_ENET_CPSW0,
        .txChId      = ENET_DMA_TX_CH_CPSW,
        .txPktNum    = ENET_DMA_TX_CH_CPSW_NUM_PKTS,
        .rxChId      = ENET_DMA_RX_CH_CPSW,
        .rxPktNum    = ENET_DMA_RX_CH_CPSW_NUM_PKTS,
    },
};

static EnetApp_Obj gEnetApp;

static uint8_t gEnetAppTaskStackRx[ENETAPP_PER_MAX][ENETAPP_TASK_STACK_SZ] __attribute__ ((aligned(32)));

/* ========================================================================== */
/*                          Function Definitions                              */
/* ========================================================================== */

static void EnetApp_showMenu(void)
{
    EnetAppUtils_print("\nEnet ICSSG + CPSW Menu:\r\n");
    EnetAppUtils_print(" 's'  -  Print statistics\r\n");
    EnetAppUtils_print(" 'r'  -  Reset statistics\r\n");
    EnetAppUtils_print(" 'x'  -  Stop the test\r\n\n");
}

void EnetApp_mainTask(void *args)
{
    char option;
    uint32_t i;
    int32_t status;

    DebugP_log("==============================================\r\n");
    DebugP_log("    ENET LAYER2 ICSSG + CPSW ON ONE CORE      \r\n");
    DebugP_log("==============================================\r\n");

    memset(&gEnetApp, 0, sizeof(gEnetApp));
    gEnetApp.run = true;
    gEnetApp.coreId = EnetSoc_getCoreId();

    for (i = 0U; i < ENETAPP_PER_MAX; i++)
    {
        EnetApp_PerCtxt *perCtxt = &gEnetApp.perCtxt[i];

        perCtxt->info = &gEnetAppPortInfo[i];
        EnetApp_getEnetInstInfo(perCtxt->info->enetInstIdx, &perCtxt->enetType, &perCtxt->instId);
        EnetApp_getEnetInstMacInfo(perCtxt->enetType, perCtxt->instId,
                                   perCtxt->macPort, &perCtxt->macPortNum);
        EnetQueue_initQ(&perCtxt->txFreePktInfoQ);
    }

    status = EnetApp_open();
    if (status == ENET_SOK)
    {
        EnetApp_showMenu();
        while (true)
        {
            option = ' ';
            DebugP_scanf("%c", &option);
            if (option == 'x')
            {
                EnetAppUtils_print("Stopping...\r\n");
                gEnetApp.run = false;
                break;
            }
            else if (option == 's')
            {
                EnetApp_printStats();
            }
            else if (option == 'r')
            {
                for (i = 0U; i < ENETAPP_PER_MAX; i++)
                {
                    gEnetApp.perCtxt[i].rxPktCnt  = 0U;
                    gEnetApp.perCtxt[i].txPktCnt  = 0U;
                    gEnetApp.perCtxt[i].txDropCnt = 0U;
                }
            }
            else
            {
                EnetAppUtils_print("Invalid option, try again...\r\n");
                EnetApp_showMenu();
            }
            TaskP_yield();
        }

        EnetApp_printStats();

        /* Wait until the RX tasks have exited */
        for (i = 0U; i < ENETAPP_PER_MAX; i++)
        {
            SemaphoreP_post(&gEnetApp.perCtxt[i].rxSemObj);
            SemaphoreP_pend(&gEnetApp.perCtxt[i].rxDoneSemObj, SystemP_WAIT_FOREVER);
        }
    }
    else
    {
        EnetAppUtils_print("Failed to open the Ethernet ports: %d\r\n", status);
    }

    EnetApp_close();
}

/* Called by the generated ICSSG driver open code */
void EnetApp_updateIcssgInitCfg(Enet_Type enetType, uint32_t instId, Icssg_Cfg *icssgCfg)
{
    /* Defaults from SysConfig are used */
}

static void EnetApp_portLinkStatusChangeCb(Enet_MacPort macPort,
                                           bool isLinkUp,
                                           void *appArg)
{
    EnetAppUtils_print("CPSW MAC port %u: link %s\r\n",
                       ENET_MACPORT_ID(macPort), isLinkUp ? "up" : "down");
}

/* Called by the generated CPSW driver open code */
void EnetApp_updateCpswInitCfg(Enet_Type enetType, uint32_t instId, Cpsw_Cfg *cpswCfg)
{
    cpswCfg->portLinkStatusChangeCb    = &EnetApp_portLinkStatusChangeCb;
    cpswCfg->portLinkStatusChangeCbArg = &gEnetApp;
}

static void EnetApp_asyncIoctlCb(Enet_Event evt,
                                 uint32_t evtNum,
                                 void *evtCbArgs,
                                 void *arg1,
                                 void *arg2)
{
    EnetApp_PerCtxt *perCtxt = (EnetApp_PerCtxt *)evtCbArgs;

    SemaphoreP_post(&perCtxt->asyncIoctlSemObj);
}

static void EnetApp_rxIsrFxn(void *appData)
{
    EnetApp_PerCtxt *perCtxt = (EnetApp_PerCtxt *)appData;

    SemaphoreP_post(&perCtxt->rxSemObj);
}

static int32_t EnetApp_open(void)
{
    TaskP_Params taskParams;
    uint32_t i;
    int32_t status = ENET_SOK;

    EnetAppUtils_print("\nEnable peripheral clocks\r\n");
    for (i = 0U; i < ENETAPP_PER_MAX; i++)
    {
        EnetAppUtils_enableClocks(gEnetApp.perCtxt[i].enetType, gEnetApp.perCtxt[i].instId);
    }

    /* One driver init for both CPSW and ICSSG */
    EnetApp_driverInit();

    for (i = 0U; (i < ENETAPP_PER_MAX) && (status == ENET_SOK); i++)
    {
        EnetApp_PerCtxt *perCtxt = &gEnetApp.perCtxt[i];

        EnetAppUtils_print("%s: Open Enet (type %u, instance %u)\r\n",
                           perCtxt->info->name, perCtxt->enetType, perCtxt->instId);

        status = SemaphoreP_constructBinary(&perCtxt->asyncIoctlSemObj, 0);
        DebugP_assert(SystemP_SUCCESS == status);

        status = EnetApp_driverOpen(perCtxt->enetType, perCtxt->instId);
        if (status != ENET_SOK)
        {
            EnetAppUtils_print("%s: Failed to open Enet: %d\r\n", perCtxt->info->name, status);
            break;
        }

        EnetApp_acquireHandleInfo(perCtxt->enetType, perCtxt->instId, &perCtxt->handleInfo);
        EnetApp_coreAttach(perCtxt->enetType, perCtxt->instId, gEnetApp.coreId, &perCtxt->attachInfo);

        if (Enet_isIcssFamily(perCtxt->enetType))
        {
            Enet_registerEventCb(perCtxt->handleInfo.hEnet,
                                 ENET_EVT_ASYNC_CMD_RESP,
                                 0U,
                                 EnetApp_asyncIoctlCb,
                                 (void *)perCtxt);
        }

        status = SemaphoreP_constructBinary(&perCtxt->rxSemObj, 0);
        DebugP_assert(SystemP_SUCCESS == status);
        status = SemaphoreP_constructBinary(&perCtxt->rxDoneSemObj, 0);
        DebugP_assert(SystemP_SUCCESS == status);

        status = EnetApp_openDma(perCtxt);
        if (status != ENET_SOK)
        {
            EnetAppUtils_print("%s: Failed to open DMA: %d\r\n", perCtxt->info->name, status);
            break;
        }

        TaskP_Params_init(&taskParams);
        taskParams.priority  = ENETAPP_RX_TASK_PRI;
        taskParams.stack     = &gEnetAppTaskStackRx[i][0U];
        taskParams.stackSize = ENETAPP_TASK_STACK_SZ;
        taskParams.args      = (void *)perCtxt;
        taskParams.name      = "Rx Task";
        taskParams.taskMain  = &EnetApp_rxTask;

        status = TaskP_construct(&perCtxt->rxTaskObj, &taskParams);
        DebugP_assert(SystemP_SUCCESS == status);
    }

    for (i = 0U; (i < ENETAPP_PER_MAX) && (status == ENET_SOK); i++)
    {
        status = EnetApp_waitForLinkUp(&gEnetApp.perCtxt[i]);
    }

    if (status == ENET_SOK)
    {
        for (i = 0U; i < ENETAPP_PER_MAX; i++)
        {
            EnetAppUtils_print("%s: MAC address ", gEnetApp.perCtxt[i].info->name);
            EnetAppUtils_printMacAddr(&gEnetApp.perCtxt[i].macAddr[0U]);
        }
    }

    return status;
}

static void EnetApp_close(void)
{
    uint32_t i;

    for (i = 0U; i < ENETAPP_PER_MAX; i++)
    {
        EnetApp_PerCtxt *perCtxt = &gEnetApp.perCtxt[i];

        if (perCtxt->handleInfo.hEnet == NULL)
        {
            continue;
        }

        EnetAppUtils_print("%s: Close\r\n", perCtxt->info->name);

        EnetApp_closeDma(perCtxt);

        EnetApp_coreDetach(perCtxt->enetType, perCtxt->instId, gEnetApp.coreId,
                           perCtxt->attachInfo.coreKey);

        /* Closes the Enet driver of this instance (and unregisters its event callbacks) */
        EnetApp_releaseHandleInfo(perCtxt->enetType, perCtxt->instId);
        perCtxt->handleInfo.hEnet = NULL;

        SemaphoreP_destruct(&perCtxt->rxSemObj);
        SemaphoreP_destruct(&perCtxt->rxDoneSemObj);
        SemaphoreP_destruct(&perCtxt->asyncIoctlSemObj);
    }

    EnetApp_driverDeInit();

    for (i = 0U; i < ENETAPP_PER_MAX; i++)
    {
        EnetAppUtils_disableClocks(gEnetApp.perCtxt[i].enetType, gEnetApp.perCtxt[i].instId);
    }
}

static int32_t EnetApp_openDma(EnetApp_PerCtxt *perCtxt)
{
    EnetApp_GetDmaHandleInArgs txInArgs;
    EnetApp_GetTxDmaHandleOutArgs txChInfo;
    EnetApp_GetDmaHandleInArgs rxInArgs;
    EnetApp_GetRxDmaHandleOutArgs rxChInfo;
    EnetDma_PktQ rxFreeQ;
    EnetDma_Pkt *pktInfo;
    uint32_t scatterSegments[] = { ENET_MEM_LARGE_POOL_PKT_SIZE };
    uint32_t i;
    int32_t status = ENET_SOK;

    /* TX channel. Channel indices are global across CPSW and ICSSG */
    txInArgs.enetType = perCtxt->enetType;
    txInArgs.instId   = perCtxt->instId;
    txInArgs.cbArg    = NULL;
    txInArgs.notifyCb = NULL;
    EnetApp_getTxDmaHandle(perCtxt->info->txChId, &txInArgs, &txChInfo);
    perCtxt->hTxCh = txChInfo.hTxCh;
    if (perCtxt->hTxCh == NULL)
    {
        status = ENET_EFAIL;
    }

    if (status == ENET_SOK)
    {
        for (i = 0U; (i < perCtxt->info->txPktNum) && (i < ENETAPP_TX_PKT_MAX); i++)
        {
            pktInfo = EnetMem_allocEthPkt(perCtxt,
                                          ENETDMA_CACHELINE_ALIGNMENT,
                                          ENET_ARRAYSIZE(scatterSegments),
                                          scatterSegments);
            EnetAppUtils_assert(pktInfo != NULL);
            ENET_UTILS_SET_PKT_APP_STATE(&pktInfo->pktState, ENET_PKTSTATE_APP_WITH_FREEQ);
            EnetQueue_enq(&perCtxt->txFreePktInfoQ, &pktInfo->node);
        }
    }

    /* RX flow */
    if (status == ENET_SOK)
    {
        rxInArgs.enetType = perCtxt->enetType;
        rxInArgs.instId   = perCtxt->instId;
        rxInArgs.notifyCb = EnetApp_rxIsrFxn;
        rxInArgs.cbArg    = perCtxt;
        EnetApp_getRxDmaHandle(perCtxt->info->rxChId, &rxInArgs, &rxChInfo);
        perCtxt->hRxCh = rxChInfo.hRxCh;
        if ((perCtxt->hRxCh == NULL) || (rxChInfo.numValidMacAddress == 0U))
        {
            status = ENET_EFAIL;
        }
    }

    if (status == ENET_SOK)
    {
        EnetUtils_copyMacAddr(&perCtxt->macAddr[0U], &rxChInfo.macAddr[0U][0U]);

        /* Submit all RX buffers to DMA */
        EnetQueue_initQ(&rxFreeQ);
        for (i = 0U; i < perCtxt->info->rxPktNum; i++)
        {
            pktInfo = EnetMem_allocEthPkt(perCtxt,
                                          ENETDMA_CACHELINE_ALIGNMENT,
                                          ENET_ARRAYSIZE(scatterSegments),
                                          scatterSegments);
            EnetAppUtils_assert(pktInfo != NULL);
            ENET_UTILS_SET_PKT_APP_STATE(&pktInfo->pktState, ENET_PKTSTATE_APP_WITH_FREEQ);
            EnetQueue_enq(&rxFreeQ, &pktInfo->node);
        }

        EnetAppUtils_validatePacketState(&rxFreeQ,
                                         ENET_PKTSTATE_APP_WITH_FREEQ,
                                         ENET_PKTSTATE_APP_WITH_DRIVER);
        status = EnetDma_submitRxPktQ(perCtxt->hRxCh, &rxFreeQ);
    }

    return status;
}

static void EnetApp_retrieveFreeTxPkts(EnetApp_PerCtxt *perCtxt)
{
    EnetDma_PktQ txFreeQ;
    EnetDma_Pkt *pktInfo;
    int32_t status;

    EnetQueue_initQ(&txFreeQ);
    status = EnetDma_retrieveTxPktQ(perCtxt->hTxCh, &txFreeQ);
    if (status == ENET_SOK)
    {
        pktInfo = (EnetDma_Pkt *)EnetQueue_deq(&txFreeQ);
        while (pktInfo != NULL)
        {
            EnetDma_checkPktState(&pktInfo->pktState,
                                  ENET_PKTSTATE_MODULE_APP,
                                  ENET_PKTSTATE_APP_WITH_DRIVER,
                                  ENET_PKTSTATE_APP_WITH_FREEQ);
            EnetQueue_enq(&perCtxt->txFreePktInfoQ, &pktInfo->node);
            pktInfo = (EnetDma_Pkt *)EnetQueue_deq(&txFreeQ);
        }
    }
}

static void EnetApp_closeDma(EnetApp_PerCtxt *perCtxt)
{
    EnetDma_PktQ fqPktInfoQ;
    EnetDma_PktQ cqPktInfoQ;

    if (perCtxt->hRxCh != NULL)
    {
        EnetQueue_initQ(&fqPktInfoQ);
        EnetQueue_initQ(&cqPktInfoQ);
        EnetApp_closeRxDma(perCtxt->info->rxChId,
                           perCtxt->handleInfo.hEnet,
                           perCtxt->attachInfo.coreKey,
                           gEnetApp.coreId,
                           &fqPktInfoQ,
                           &cqPktInfoQ);
        EnetAppUtils_freePktInfoQ(&fqPktInfoQ);
        EnetAppUtils_freePktInfoQ(&cqPktInfoQ);
        perCtxt->hRxCh = NULL;
    }

    if (perCtxt->hTxCh != NULL)
    {
        EnetApp_retrieveFreeTxPkts(perCtxt);

        EnetQueue_initQ(&fqPktInfoQ);
        EnetQueue_initQ(&cqPktInfoQ);
        EnetApp_closeTxDma(perCtxt->info->txChId,
                           perCtxt->handleInfo.hEnet,
                           perCtxt->attachInfo.coreKey,
                           gEnetApp.coreId,
                           &fqPktInfoQ,
                           &cqPktInfoQ);
        EnetAppUtils_freePktInfoQ(&fqPktInfoQ);
        EnetAppUtils_freePktInfoQ(&cqPktInfoQ);
        perCtxt->hTxCh = NULL;
    }

    EnetAppUtils_freePktInfoQ(&perCtxt->txFreePktInfoQ);
}

static int32_t EnetApp_setIcssgPortForward(EnetApp_PerCtxt *perCtxt, Enet_MacPort macPort)
{
    IcssgMacPort_SetPortStateInArgs setPortStateInArgs;
    Enet_IoctlPrms prms;
    int32_t status;

    setPortStateInArgs.macPort   = macPort;
    setPortStateInArgs.portState = ICSSG_PORT_STATE_FORWARD;
    ENET_IOCTL_SET_IN_ARGS(&prms, &setPortStateInArgs);

    ENET_IOCTL(perCtxt->handleInfo.hEnet, gEnetApp.coreId, ICSSG_PER_IOCTL_SET_PORT_STATE, &prms, status);
    if (status == ENET_SINPROGRESS)
    {
        /* Wait for the asynchronous IOCTL to complete */
        Enet_poll(perCtxt->handleInfo.hEnet, ENET_EVT_ASYNC_CMD_RESP, NULL, 0U);
        SemaphoreP_pend(&perCtxt->asyncIoctlSemObj, SystemP_WAIT_FOREVER);
        status = ENET_SOK;
    }

    return status;
}

static int32_t EnetApp_waitForLinkUp(EnetApp_PerCtxt *perCtxt)
{
    Enet_IoctlPrms prms;
    Enet_MacPort macPort;
    bool linked;
    uint32_t i;
    int32_t status = ENET_SOK;

    EnetAppUtils_print("%s: Waiting for link up...\r\n", perCtxt->info->name);

    for (i = 0U; (i < perCtxt->macPortNum) && (status == ENET_SOK); i++)
    {
        macPort = perCtxt->macPort[i];
        linked = false;

        while (gEnetApp.run && !linked)
        {
            ENET_IOCTL_SET_INOUT_ARGS(&prms, &macPort, &linked);
            ENET_IOCTL(perCtxt->handleInfo.hEnet, gEnetApp.coreId, ENET_PER_IOCTL_IS_PORT_LINK_UP, &prms, status);
            if (status != ENET_SOK)
            {
                EnetAppUtils_print("%s: Failed to get link status: %d\r\n", perCtxt->info->name, status);
                break;
            }

            if (!linked)
            {
                ClockP_usleep(1000U);
            }
        }

        if ((status == ENET_SOK) && linked)
        {
            EnetAppUtils_print("%s: MAC port %u link is up\r\n", perCtxt->info->name, ENET_MACPORT_ID(macPort));

            if (Enet_isIcssFamily(perCtxt->enetType))
            {
                status = EnetApp_setIcssgPortForward(perCtxt, macPort);
                if (status != ENET_SOK)
                {
                    EnetAppUtils_print("%s: Failed to set port state: %d\r\n", perCtxt->info->name, status);
                }
            }
        }
    }

    return status;
}

static void EnetApp_rxTask(void *args)
{
    EnetApp_PerCtxt *perCtxt = (EnetApp_PerCtxt *)args;
    EnetDma_PktQ rxReadyQ;
    EnetDma_PktQ rxFreeQ;
    EnetDma_PktQ txSubmitQ;
    EnetDma_Pkt *rxPktInfo;
    EnetDma_Pkt *txPktInfo;
    EthFrame *rxFrame;
    EthFrame *txFrame;
    uint32_t pktLen;
    int32_t status = ENET_SOK;

    while ((status == ENET_SOK) && gEnetApp.run)
    {
        SemaphoreP_pend(&perCtxt->rxSemObj, SystemP_WAIT_FOREVER);
        if (!gEnetApp.run)
        {
            break;
        }

        EnetQueue_initQ(&rxReadyQ);
        EnetQueue_initQ(&rxFreeQ);
        EnetQueue_initQ(&txSubmitQ);

        status = EnetDma_retrieveRxPktQ(perCtxt->hRxCh, &rxReadyQ);
        if (status != ENET_SOK)
        {
            EnetAppUtils_print("%s: Failed to retrieve RX packets: %d\r\n", perCtxt->info->name, status);
            break;
        }

        EnetApp_retrieveFreeTxPkts(perCtxt);

        rxPktInfo = (EnetDma_Pkt *)EnetQueue_deq(&rxReadyQ);
        while (rxPktInfo != NULL)
        {
            EnetDma_checkPktState(&rxPktInfo->pktState,
                                  ENET_PKTSTATE_MODULE_APP,
                                  ENET_PKTSTATE_APP_WITH_DRIVER,
                                  ENET_PKTSTATE_APP_WITH_READYQ);
            perCtxt->rxPktCnt++;

            /* Buffers come from the large pool, so a packet fits in one segment */
            rxFrame = (EthFrame *)rxPktInfo->sgList.list[0U].bufPtr;
            pktLen = rxPktInfo->sgList.list[0U].segmentFilledLen;

            txPktInfo = (EnetDma_Pkt *)EnetQueue_deq(&perCtxt->txFreePktInfoQ);
            if ((txPktInfo != NULL) && (rxPktInfo->sgList.numScatterSegments == 1U))
            {
                txFrame = (EthFrame *)txPktInfo->sgList.list[0U].bufPtr;
                memcpy(txFrame->hdr.dstMac, rxFrame->hdr.srcMac, ENET_MAC_ADDR_LEN);
                memcpy(txFrame->hdr.srcMac, &perCtxt->macAddr[0U], ENET_MAC_ADDR_LEN);
                txFrame->hdr.etherType = rxFrame->hdr.etherType;
                memcpy(&txFrame->payload[0U], &rxFrame->payload[0U], pktLen - sizeof(EthFrameHeader));

                txPktInfo->sgList.list[0U].segmentFilledLen = pktLen;
                txPktInfo->sgList.numScatterSegments = 1U;
                txPktInfo->chkSumInfo = 0U;
                txPktInfo->appPriv    = &gEnetApp;
                txPktInfo->txPortNum  = ENET_MAC_PORT_INV;
                txPktInfo->txPktTc    = 0U;
                txPktInfo->tsInfo.enableHostTxTs = false;

                EnetDma_checkPktState(&txPktInfo->pktState,
                                      ENET_PKTSTATE_MODULE_APP,
                                      ENET_PKTSTATE_APP_WITH_FREEQ,
                                      ENET_PKTSTATE_APP_WITH_DRIVER);
                EnetQueue_enq(&txSubmitQ, &txPktInfo->node);
                perCtxt->txPktCnt++;
            }
            else
            {
                if (txPktInfo != NULL)
                {
                    EnetQueue_enq(&perCtxt->txFreePktInfoQ, &txPktInfo->node);
                }
                perCtxt->txDropCnt++;
            }

            EnetDma_checkPktState(&rxPktInfo->pktState,
                                  ENET_PKTSTATE_MODULE_APP,
                                  ENET_PKTSTATE_APP_WITH_READYQ,
                                  ENET_PKTSTATE_APP_WITH_FREEQ);
            EnetQueue_enq(&rxFreeQ, &rxPktInfo->node);
            rxPktInfo = (EnetDma_Pkt *)EnetQueue_deq(&rxReadyQ);
        }

        status = EnetDma_submitTxPktQ(perCtxt->hTxCh, &txSubmitQ);
        if (status != ENET_SOK)
        {
            EnetAppUtils_print("%s: Failed to submit TX packets: %d\r\n", perCtxt->info->name, status);
        }

        EnetAppUtils_validatePacketState(&rxFreeQ,
                                         ENET_PKTSTATE_APP_WITH_FREEQ,
                                         ENET_PKTSTATE_APP_WITH_DRIVER);
        status = EnetDma_submitRxPktQ(perCtxt->hRxCh, &rxFreeQ);
        if (status != ENET_SOK)
        {
            EnetAppUtils_print("%s: Failed to submit RX packets: %d\r\n", perCtxt->info->name, status);
        }
    }

    SemaphoreP_post(&perCtxt->rxDoneSemObj);
    TaskP_exit();
}

static void EnetApp_printStats(void)
{
    uint32_t i;

    EnetAppUtils_print("\n%-16s %12s %12s %12s\r\n", "Port", "RX packets", "TX packets", "TX dropped");
    for (i = 0U; i < ENETAPP_PER_MAX; i++)
    {
        EnetApp_PerCtxt *perCtxt = &gEnetApp.perCtxt[i];

        EnetAppUtils_print("%-16s %12u %12u %12u\r\n", perCtxt->info->name,
                           perCtxt->rxPktCnt, perCtxt->txPktCnt, perCtxt->txDropCnt);
    }
}
