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
 * \file  app_enetconfighandler.c
 *
 * \brief Peripheral configuration callbacks called by the generated Enet
 *        driver open code, one for ICSSG and one for CPSW.
 */

/* ========================================================================== */
/*                             Include Files                                  */
/* ========================================================================== */

#include <stdint.h>
#include <enet_apputils.h>
#include <include/per/icssg.h>
#include <include/per/cpsw.h>
#include "ti_drivers_config.h"
#include "ti_enet_config.h"
#include "ti_enet_open_close.h"

/* ========================================================================== */
/*                           Macros & Typedefs                                */
/* ========================================================================== */

#define MII_LINK0_EVENT      41
#define MII_LINK1_EVENT      53

/* ========================================================================== */
/*                          Function Declarations                             */
/* ========================================================================== */

#if (ENET_SYSCFG_ENABLE_MDIO_MANUALMODE_ICSSG == 0U)
static void EnetApp_mdioLinkStatusChange(Icssg_MdioLinkStateChangeInfo *info,
                                         void *appArg);
#endif

static void EnetApp_portLinkStatusChangeCb(Enet_MacPort macPort,
                                           bool isLinkUp,
                                           void *appArg);

/* ========================================================================== */
/*                          Function Definitions                              */
/* ========================================================================== */

#if (ENET_SYSCFG_ENABLE_MDIO_MANUALMODE_ICSSG == 0U)
static void EnetApp_updateMdioLinkIntCfg(Enet_Type enetType, uint32_t instId, Icssg_mdioLinkIntCfg *mdioLinkIntCfg)
{
    /* INTC mapping for the PRU to R5F interrupts, from the PRUICSS instance
     * (CONFIG_PRU_ICSS1) in SysConfig */
    mdioLinkIntCfg->prussIntcInitData = &icss0_intc_initdata;
    mdioLinkIntCfg->coreIntrNum  = 254;
    mdioLinkIntCfg->pruEvtNum[0] = MII_LINK0_EVENT;
    mdioLinkIntCfg->pruEvtNum[1] = MII_LINK1_EVENT;
    mdioLinkIntCfg->isPulseIntr  = 0;
    mdioLinkIntCfg->intrPrio     = 15;
}

static void EnetApp_mdioLinkStatusChange(Icssg_MdioLinkStateChangeInfo *info,
                                         void *appArg)
{
    EnetAppUtils_print("ICSSG link status changed. PHY: 0x%x, state: %s\r\n",
                       info->phyAddr, info->isLinked ? "up" : "down");
}
#endif

/* Called by the generated ICSSG driver open code */
void EnetApp_updateIcssgInitCfg(Enet_Type enetType, uint32_t instId, Icssg_Cfg *icssgCfg)
{
#if (ENET_SYSCFG_ENABLE_MDIO_MANUALMODE_ICSSG == 1U)
    icssgCfg->mdioLinkIntCfg.mdioLinkStateChangeCb    = NULL;
    icssgCfg->mdioLinkIntCfg.mdioLinkStateChangeCbArg = NULL;
#else
    icssgCfg->mdioLinkIntCfg.mdioLinkStateChangeCb    = &EnetApp_mdioLinkStatusChange;
    icssgCfg->mdioLinkIntCfg.mdioLinkStateChangeCbArg = NULL;
    EnetApp_updateMdioLinkIntCfg(enetType, instId, &icssgCfg->mdioLinkIntCfg);
#endif
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
    cpswCfg->portLinkStatusChangeCbArg = NULL;
}
