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
 * \file  app_main.c
 *
 * \brief lwIP example running ICSSG and CPSW from the same R5F core.
 *
 * Three Ethernet ports are opened by one core, each with its own lwIP netif:
 *  - NETIF_INST_ID0: ICSSG1 dual-MAC port 1 (Enet instance CONFIG_ENET_ICSS0)
 *  - NETIF_INST_ID1: ICSSG1 dual-MAC port 2 (Enet instance CONFIG_ENET_ICSS1)
 *  - NETIF_INST_ID2: CPSW MAC port 1        (Enet instance CONFIG_ENET_CPSW0)
 *
 * Every netif gets its address from DHCP. A TCP echo server listens on port
 * 8888 of every netif.
 */

/* ========================================================================== */
/*                             Include Files                                  */
/* ========================================================================== */

#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
/* lwIP core includes */
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/tcpip.h"
#include "lwip/dhcp.h"

#include <kernel/dpl/TaskP.h>
#include <kernel/dpl/ClockP.h>

#include <enet_apputils.h>
#include <enet_board.h>
#include "ti_board_config.h"
#include "ti_board_open_close.h"
#include "ti_drivers_open_close.h"
#include "ti_enet_config.h"
#include "ti_enet_open_close.h"
#include "ti_enet_lwipif.h"
#include "app_tcpserver.h"

/* ========================================================================== */
/*                         Structure Declarations                             */
/* ========================================================================== */

typedef struct EnetApp_AppEnetInfo
{
    /* Peripheral type */
    Enet_Type enetType;

    /* Peripheral instance */
    uint32_t instId;
} EnetApp_AppEnetInfo;

/* ========================================================================== */
/*                          Function Declarations                             */
/* ========================================================================== */

static void App_printCpuLoad(void);

static void App_tcpipInitCompleteCb(void *pArg);

static void App_setupNetif(void);

static void App_allocateIPAddress(void);

static void App_setupNetworkStack(void);

static void App_shutdownNetworkStack(void);

static void App_netifStatusChangeCb(struct netif *pNetif);

static void App_netifLinkChangeCb(struct netif *pNetif);

static int32_t App_isNetworkUp(struct netif *pNetif);

/* ========================================================================== */
/*                            Global Variables                                */
/* ========================================================================== */

/* Names of the netifs, indexed by the global netif index from SysConfig */
static const char *gAppNetifNames[ENET_SYSCFG_NETIF_COUNT] =
{
    [NETIF_INST_ID0] = "ICSSG port 1",
    [NETIF_INST_ID1] = "ICSSG port 2",
    [NETIF_INST_ID2] = "CPSW port 1",
};

/* DHCP state of each netif */
static struct dhcp gAppNetifDhcp[ENET_SYSCFG_NETIF_COUNT];

static struct netif *gAppNetif[ENET_SYSCFG_NETIF_COUNT];

/* ICSSG instances come first, then CPSW: CONFIG_ENET_ICSS0 .. CONFIG_ENET_CPSW0 */
static EnetApp_AppEnetInfo gEnetAppParams[ENET_SYSCFG_MAX_ENET_INSTANCES];

/* Handle to the application interface of the lwIP interface layer */
static LwipifEnetApp_Handle gLwipIfApp = NULL;

/* ========================================================================== */
/*                          Function Definitions                              */
/* ========================================================================== */

int appMain(void *args)
{
    int32_t status = ENET_SOK;
    uint32_t i;

    DebugP_log("=================================\r\n");
    DebugP_log("  ICSSG + CPSW LWIP TCP ECHO SERVER\r\n");
    DebugP_log("=================================\r\n");

    /* Enable the clocks of every Enet instance, ICSSG and CPSW */
    for (i = 0U; i < ENET_SYSCFG_MAX_ENET_INSTANCES; i++)
    {
        EnetApp_getEnetInstInfo(CONFIG_ENET_ICSS0 + i, &gEnetAppParams[i].enetType, &gEnetAppParams[i].instId);
        EnetAppUtils_enableClocks(gEnetAppParams[i].enetType, gEnetAppParams[i].instId);
    }

    /* One driver init for both CPSW and ICSSG, then open each instance */
    EnetApp_driverInit();
    for (i = 0U; i < ENET_SYSCFG_MAX_ENET_INSTANCES; i++)
    {
        status = EnetApp_driverOpen(gEnetAppParams[i].enetType, gEnetAppParams[i].instId);
        if (status != ENET_SOK)
        {
            EnetAppUtils_print("Failed to open ENET[%u]: %d\r\n", i, status);
            EnetAppUtils_assert(status == ENET_SOK);
        }
    }

    App_setupNetworkStack();

    /* Wait for at least one netif to get an IP address */
    uint32_t netupMask = 0U;
    while (netupMask == 0U)
    {
        for (i = 0U; i < ENET_SYSCFG_NETIF_COUNT; i++)
        {
            if (App_isNetworkUp(gAppNetif[i]))
            {
                netupMask |= (1U << i);
            }
            else
            {
                DebugP_log("[%s] Waiting for network UP ...\r\n", gAppNetifNames[i]);
            }
            ClockP_sleep(2);
        }
    }

    DebugP_log("Network is UP ...\r\n");
    ClockP_sleep(2);
    AppTcp_startServer();

    while (1)
    {
        ClockP_usleep(1000);
        App_printCpuLoad();
    }

    App_shutdownNetworkStack();
    EnetApp_driverDeInit();
    return 0;
}

static void App_setupNetworkStack(void)
{
    sys_sem_t initSem;
    const err_t err = sys_sem_new(&initSem, 0);
    EnetAppUtils_assert(err == ERR_OK);

    tcpip_init(App_tcpipInitCompleteCb, &initSem);

    /* Wait for the TCP/IP initialization to complete */
    sys_sem_wait(&initSem);
    sys_sem_free(&initSem);
}

static void App_shutdownNetworkStack(void)
{
    for (uint32_t netifIdx = 0U; netifIdx < ENET_SYSCFG_NETIF_COUNT; netifIdx++)
    {
        LwipifEnetApp_netifClose(gLwipIfApp, netifIdx);
    }
}

static void App_tcpipInitCompleteCb(void *pArg)
{
    sys_sem_t *pSem = (sys_sem_t *)pArg;
    EnetAppUtils_assert(pArg != NULL);

    /* Init randomizer again (seed per thread) */
    srand((unsigned int)sys_now() / 1000);

    App_setupNetif();

    App_allocateIPAddress();

    sys_sem_signal(pSem);
}

static void App_setupNetif(void)
{
    ip4_addr_t ipaddr, netmask, gw;

    ip4_addr_set_zero(&gw);
    ip4_addr_set_zero(&ipaddr);
    ip4_addr_set_zero(&netmask);

    DebugP_log("Starting lwIP, local interface IPs are dhcp-enabled\r\n");
    gLwipIfApp = LwipifEnetApp_getHandle();

    /* Netif indices are global: ICSSG netifs first, then CPSW */
    for (uint32_t netifIdx = 0U; netifIdx < ENET_SYSCFG_NETIF_COUNT; netifIdx++)
    {
        gAppNetif[netifIdx] = LwipifEnetApp_netifOpen(gLwipIfApp, netifIdx, &ipaddr, &netmask, &gw);
        EnetAppUtils_assert(gAppNetif[netifIdx] != NULL);
        LwipifEnetApp_startSchedule(gLwipIfApp, gAppNetif[netifIdx]);
        netif_set_status_callback(gAppNetif[netifIdx], App_netifStatusChangeCb);
        netif_set_link_callback(gAppNetif[netifIdx], App_netifLinkChangeCb);
        netif_set_up(gAppNetif[netifIdx]);
        DebugP_log("[%s] netif %c%c%u opened\r\n", gAppNetifNames[netifIdx],
                   gAppNetif[netifIdx]->name[0], gAppNetif[netifIdx]->name[1],
                   gAppNetif[netifIdx]->num);
    }
}

static void App_allocateIPAddress(void)
{
    sys_lock_tcpip_core();
    for (uint32_t netifIdx = 0U; netifIdx < ENET_SYSCFG_NETIF_COUNT; netifIdx++)
    {
        dhcp_set_struct(gAppNetif[netifIdx], &gAppNetifDhcp[netifIdx]);

        const err_t err = dhcp_start(gAppNetif[netifIdx]);
        EnetAppUtils_assert(err == ERR_OK);
    }
    sys_unlock_tcpip_core();
}

static const char *App_getNetifName(struct netif *pNetif)
{
    const int32_t netifIdx = LwipifEnetApp_getNetifIdx(gLwipIfApp, pNetif);

    return ((netifIdx >= 0) && (netifIdx < (int32_t)ENET_SYSCFG_NETIF_COUNT)) ?
           gAppNetifNames[netifIdx] : "?";
}

static void App_netifStatusChangeCb(struct netif *pNetif)
{
    if (netif_is_up(pNetif))
    {
        DebugP_log("[%s] Enet IF UP Event. Local interface IP:%s\r\n",
                   App_getNetifName(pNetif), ip4addr_ntoa(netif_ip4_addr(pNetif)));
    }
    else
    {
        DebugP_log("[%s] Enet IF DOWN Event\r\n", App_getNetifName(pNetif));
    }
}

static void App_netifLinkChangeCb(struct netif *pNetif)
{
    if (netif_is_link_up(pNetif))
    {
        DebugP_log("[%s] Network Link UP Event\r\n", App_getNetifName(pNetif));
    }
    else
    {
        DebugP_log("[%s] Network Link DOWN Event\r\n", App_getNetifName(pNetif));
    }
}

static int32_t App_isNetworkUp(struct netif *pNetif)
{
    return (netif_is_up(pNetif) && netif_is_link_up(pNetif) &&
            !ip4_addr_isany_val(*netif_ip4_addr(pNetif)));
}

static void App_printCpuLoad(void)
{
    static uint32_t startTime_ms = 0;
    const  uint32_t currTime_ms  = ClockP_getTimeUsec() / 1000;
    const  uint32_t printInterval_ms = 5000;

    if (startTime_ms == 0)
    {
        startTime_ms = currTime_ms;
    }
    else if ((currTime_ms - startTime_ms) > printInterval_ms)
    {
        const uint32_t cpuLoad = TaskP_loadGetTotalCpuLoad();

        DebugP_log(" %6d.%3ds : CPU load = %3d.%02d %%\r\n",
                   currTime_ms / 1000, currTime_ms % 1000,
                   cpuLoad / 100, cpuLoad % 100);

        startTime_ms = currTime_ms;
        TaskP_loadResetAll();
    }
}
