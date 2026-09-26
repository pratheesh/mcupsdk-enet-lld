"use strict";

/*
 *  ======== enet_combo.syscfg.js ========
 *  Support for running CPSW and ICSSG from the same core ("combined" mode).
 *
 *  When both the "Enet (CPSW)" and "Enet (ICSS)" modules are added to one
 *  SysConfig design, each module still renders its own generated code, and
 *  both outputs land in the same ti_enet_*.c/h and ti_board_config.c/h files.
 *  The two outputs define many of the same identifiers (EnetApp_driverOpen,
 *  EnetSoc_getEnetHandle, gEnetAppSysCfgObj, ENET_SYSCFG_TX_CHANNELS_NUM, ...).
 *
 *  In combined mode every identifier listed in gNamespacedIdentifiers is
 *  suffixed with the peripheral name (_Cpsw/_Icssg for code, _CPSW/_ICSSG for
 *  macros) in that peripheral's output, and the templates under
 *  common/templates/combo add a small dispatch layer that defines the public
 *  names (the ones called by the Enet LLD library and by applications) and
 *  forwards each call to the peripheral that owns the Enet type, instance,
 *  DMA channel or peripheral index.
 *
 *  Index spaces seen by the application in combined mode:
 *  - Enet instances, TX channels and RX channels of the ICSSG module come
 *    first, followed by those of the CPSW module. The name macros generated
 *    for CPSW instances and DMA channels use these global indices.
 *
 *  Resources that must be single instances are not namespaced:
 *  - Packet/descriptor/ring memory (EnetMem_getCfg, EnetUdmaMem_getCfg) is
 *    generated once by the ICSSG module, sized for both peripherals.
 *  - The MAC address pool (EnetAppSoc_getMacAddrList/releaseMacAddrList) is
 *    generated once by the ICSSG module, so the peripherals never receive the
 *    same MAC address.
 *  - lwIP netifs: the netif index macros are global (ICSSG netifs first, then
 *    CPSW netifs). Each module keeps its own netifs and lwIP tasks; the combo
 *    lwipif templates dispatch the lwIP interface API and the callbacks of the
 *    Enet lwIP interface library to the module that owns the netif, Enet type
 *    or DMA channel.
 *
 *  Settings that may differ between the modules:
 *  - Packet pool / packet-info-only memory: the single EnetMem pool is
 *    enabled when any instance enables it and is sized for all instances that
 *    enable it.
 *  - Custom board: a module with a custom board generates no board
 *    configuration; the application provides that module's board functions
 *    with the module suffix (see templates/combo/board_config_h.xdt).
 *  - MCM: enabled per module (enet_mcm.c keeps one MCM object per peripheral
 *    family when both are built).
 *
 *  The identifier list below is the set of file-scope identifiers that both
 *  modules define in their generated code (collected from the AM243x/AM64x
 *  examples). A new generated identifier missing from the list shows up as a
 *  redefinition error when building a combined design.
 */

let common = system.getScript("/common");

const icssModuleName = "/networking/enet_icss/enet_icss";
const cpswModuleName = "/networking/enet_cpsw/enet_cpsw";

const supportedSocs = ["am64x", "am243x"];

const gNamespacedIdentifiers = [
    "ENETAPP_PHY_STATEHANDLER_TASK_PRIORITY", "ENETAPP_PHY_STATEHANDLER_TASK_STACK",
    "ENETBOARD_AM64X_AM243X_EVM", "ENETBOARD_CPB_ID", "ENETBOARD_IDK_ID", "ENETBOARD_MII_ID",
    "ENETBOARD_SYSCFG_CUSTOM_BOARD", "ENETLWIP_APP_POLL_PERIOD", "ENETLWIP_PACKET_POLL_PERIOD_US",
    "ENET_BOARD_NUM_MACADDR_MAX", "ENET_GET_NUM_MAC_ADDR", "ENET_MAX_NUM_MAC_ADDR_STORED",
    "ENET_MEM_LARGE_POOL_PKT_SIZE", "ENET_MEM_MEDIUM_POOL_PKT_SIZE", "ENET_MEM_SMALL_POOL_PKT_SIZE",
    "ENET_SYSCFG_DEFAULT_NETIF_IDX", "ENET_SYSCFG_DEFAULT_NUM_RX_PKT",
    "ENET_SYSCFG_DEFAULT_NUM_TX_PKT", "ENET_SYSCFG_ENABLE_EXTPHY",
    "ENET_SYSCFG_ENABLE_MDIO_MANUALMODE", "ENET_SYSCFG_MAX_ENET_INSTANCES",
    "ENET_SYSCFG_MAX_MAC_PORTS", "ENET_SYSCFG_NETIF_COUNT", "ENET_SYSCFG_PKT_INFO_ONLY_ENABLE",
    "ENET_SYSCFG_PKT_POOL_ENABLE", "ENET_SYSCFG_RING_MON_NUM", "ENET_SYSCFG_RX_FLOWS_NUM",
    "ENET_SYSCFG_TIMESTAMP_SOURCE", "ENET_SYSCFG_TOTAL_NUM_RX_PKT", "ENET_SYSCFG_TOTAL_NUM_TX_PKT",
    "ENET_SYSCFG_TX_CHANNELS_NUM", "EnetAppDmaSysCfg_Obj", "EnetAppDmaSysCfg_Obj_s",
    "EnetAppRm_getIoctlPermissionInfo", "EnetAppRxDmaCfg_Info", "EnetAppRxDmaCfg_Info_s",
    "EnetAppRxDmaSysCfg_Obj", "EnetAppRxDmaSysCfg_Obj_s", "EnetAppSoc_fillMacAddrList",
    "EnetAppSysCfg_Obj", "EnetAppSysCfg_Obj_s", "EnetAppTxDmaCfg_Info", "EnetAppTxDmaCfg_Info_s",
    "EnetAppTxDmaSysCfg_Obj", "EnetAppTxDmaSysCfg_Obj_s", "EnetAppUtils_closeRxFlowForChIdx",
    "EnetAppUtils_openRxFlowForChIdx", "EnetAppUtils_setCommonRxFlowPrms",
    "EnetAppUtils_setCommonTxChPrms", "EnetApp_ConfigureDscpMapping", "EnetApp_DmaCfg",
    "EnetApp_DmaCfg_s", "EnetApp_MacAddrElem", "EnetApp_MacAddrElem_s", "EnetApp_MacAddrPool",
    "EnetApp_MacAddrPool_s", "EnetApp_RxChInitCfg", "EnetApp_RxChInitCfg_s", "EnetApp_TxChInitCfg",
    "EnetApp_TxChInitCfg_s", "EnetApp_acquireHandleInfo", "EnetApp_applyClassifier",
    "EnetApp_closeRxDma", "EnetApp_closeTxDma", "EnetApp_coreAttach", "EnetApp_coreDetach",
    "EnetApp_createPhyStateHandlerTask", "EnetApp_deleteClock", "EnetApp_driverClose",
    "EnetApp_driverDeInit", "EnetApp_driverInit", "EnetApp_driverOpen", "EnetApp_enablePortTsEvent",
    "EnetApp_filterPriorityPacketsCfg", "EnetApp_getEnetInstInfo", "EnetApp_getEnetInstMacInfo",
    "EnetApp_getMacAddrFromPool", "EnetApp_getMacAddress", "EnetApp_getMacPortLinkCfg",
    "EnetApp_getRxDmaHandle", "EnetApp_getRxTimeStamp", "EnetApp_getTxDmaHandle",
    "EnetApp_getUdmaInstanceHandle", "EnetApp_initLinkArgs", "EnetApp_initMdioConfig",
    "EnetApp_initPhyStateHandlerTask", "EnetApp_initializeMacAddrPool", "EnetApp_isPortLinked",
    "EnetApp_macMode2MacMii", "EnetApp_phyStateHandler", "EnetApp_postPollEvent",
    "EnetApp_releaseHandleInfo", "EnetApp_releaseMacAddrToPool", "EnetApp_retrieveFreeTxPkts",
    "EnetApp_rxPktNotifyCb", "EnetApp_setTimeStampComplete", "EnetApp_timerCb",
    "EnetApp_txPktNotifyCb", "EnetApp_updateRxChInitCfg", "EnetApp_updateTxChInitCfg",
    "EnetBoard_findPortCfg", "EnetBoard_getId", "EnetBoard_getMacAddrList",
    "EnetBoard_getMiiConfig", "EnetBoard_getPhyCfg", "EnetBoard_getPortCfg",
    "EnetBoard_setEnetControl", "EnetBoard_setupPorts", "EnetSoc_deinit", "EnetSoc_getClkFreq",
    "EnetSoc_getCoreId", "EnetSoc_getDmaHandle", "EnetSoc_getEFusedMacAddrs",
    "EnetSoc_getEnetHandle", "EnetSoc_getEnetHandleByIdx", "EnetSoc_getEnetNum",
    "EnetSoc_getHwPushCount", "EnetSoc_getIntrTriggerType", "EnetSoc_getMacPortCaps",
    "EnetSoc_getMacPortMax", "EnetSoc_getMacPortMii", "EnetSoc_getRxChPeerId",
    "EnetSoc_getRxFlowCount", "EnetSoc_getTxChCount", "EnetSoc_getTxChPeerId", "EnetSoc_init",
    "EnetSoc_isIpSupported", "EnetSoc_isValidEnetType", "EnetSoc_mapPort2QsgmiiId",
    "EnetSoc_validateQsgmiiCfg", "I2C_EEPROM_MAC_CTRL_OFFSET", "I2C_EEPROM_MAC_DATA_OFFSET",
    "IsMacAddrSet", "LWIPIF_NUM_RX_PACKET_TASKS", "LWIPIF_NUM_TX_PACKET_TASKS",
    "LWIPIF_POLL_TASK_STACK", "LWIPIF_RX_PACKET_TASK_PRI", "LWIPIF_RX_PACKET_TASK_STACK",
    "LWIPIF_TX_PACKET_TASK_PRI", "LWIPIF_TX_PACKET_TASK_STACK", "LWIP_POLL_TASK_PRI",
    "LwipifEnetAppCb_getEnetLwipIfInstInfo", "LwipifEnetAppCb_getRxHandleInfo",
    "LwipifEnetAppCb_getRxMode", "LwipifEnetAppCb_getTxHandleInfo",
    "LwipifEnetAppCb_releaseRxHandle", "LwipifEnetAppCb_releaseTxHandle", "LwipifEnetApp_Handle",
    "LwipifEnetApp_Object", "LwipifEnetApp_Object_s", "LwipifEnetApp_PollTaskInfo",
    "LwipifEnetApp_PollTaskInfo_s", "LwipifEnetApp_RxTaskInfo", "LwipifEnetApp_RxTaskInfo_s",
    "LwipifEnetApp_TaskInfo", "LwipifEnetApp_TaskInfo_s", "LwipifEnetApp_TxTaskInfo",
    "LwipifEnetApp_TxTaskInfo_s", "LwipifEnetApp_createPollTask",
    "LwipifEnetApp_createRxPktHandlerTask", "LwipifEnetApp_createTxPktHandlerTask",
    "LwipifEnetApp_getHandle", "LwipifEnetApp_getNetifFromId", "LwipifEnetApp_getNetifIdx",
    "LwipifEnetApp_getProxyArpRxChIDs", "LwipifEnetApp_getRxChIDs", "LwipifEnetApp_getTxChIDs",
    "LwipifEnetApp_netifClose", "LwipifEnetApp_netifOpen", "LwipifEnetApp_poll",
    "LwipifEnetApp_postPollLink", "LwipifEnetApp_postSemaphore", "LwipifEnetApp_rxPacketTask",
    "LwipifEnetApp_setupProxyArphandler", "LwipifEnetApp_startSchedule",
    "LwipifEnetApp_txPacketTask", "MAC_ADDR_LIST_LEN_SHARED_PER_PER", "NetifName_CPSW_SWITCH",
    "NetifName_NUM_NETIFS", "NetifName_e", "OS_TASKPRIHIGH", "UDMA_STATIC_RX_SG_ENABLE",
    "enetAppMacPortLinkCfg", "gDmaCfg", "gEnetAppSysCfgObj",
    "gEnetCpbBoard_ConfigEnetEthphy0PhyCfg", "gEnetCpbBoard_ConfigEnetEthphy1PhyCfg",
    "gEnetCpbBoard_am243x_evm_EthPort", "gEnetMacAddrPool", "gEnetPhyDrvTbl", "gEnetPhyDrvs",
    "gEnetRmResCfg", "gEnetSoc_dmaObj", "gEnetSoc_dmaObjMemInfo", "gEnetSoc_perObj", "gFreePbufArr",
    "gLwipifEnetAppObj", "g_EnetApp_dmaChParams", "pollLinkClkObj",
    /* file-scope statics defined by both SoC / board templates */
    "EnetSoc_getMcuEnetControl", "EnetBoard_setMacPort2IOExpanderCfg", "EnetApp_openRxDma",
    "EnetApp_openTxDma",
    /* lwIP interface layer (ti_enet_lwipif.c/h) */
    "RX_POOL", "NUM_NETIF_SUPPORTED_MAX", "LwipifEnetApp_getVepaRxChIDs",
    "LwipifEnetApp_setupPacketDuplicationRoute", "LwipifEnetApp_getNetifFromName",
    "LwipifEnetAppCb_pbuf_free_custom", "LwipifEnetApp_initRxPool",
];

/* Sub-templates rendered by one module only in combined mode.
 * key: sub-template key used in the module 'templates' definition */
const gSingleInstanceSubTemplates = {
    "enet_mem_config": icssModuleName,
};

/* Identifiers from the list above that the Enet LLD headers declare. When a
 * namespaced output calls its own copy of one of these, it needs a prototype
 * for the renamed function; NAME is replaced by the renamed identifier. */
const gLibraryPrototypes = {
    "EnetAppRm_getIoctlPermissionInfo": "const EnetRm_IoctlPermissionTable *NAME(Enet_Type enetType);",
    "EnetAppSoc_fillMacAddrList": "int32_t NAME(uint8_t macAddr[][ENET_MAC_ADDR_LEN], uint32_t maxMacEntries, uint32_t *pAvailMacEntries);",
    "EnetAppUtils_setCommonRxFlowPrms": "void NAME(EnetUdma_OpenRxFlowPrms *pRxChPrms);",
    "EnetAppUtils_setCommonTxChPrms": "void NAME(EnetUdma_OpenTxChPrms *pTxChPrms);",
    "EnetApp_acquireHandleInfo": "void NAME(Enet_Type enetType, uint32_t instId, EnetApp_HandleInfo *handleInfo);",
    "EnetApp_closeRxDma": "void NAME(uint32_t enetRxDmaChId, Enet_Handle hEnet, uint32_t coreKey, uint32_t coreId, EnetDma_PktQ *fqPktInfoQ, EnetDma_PktQ *cqPktInfoQ);",
    "EnetApp_closeTxDma": "void NAME(uint32_t enetTxDmaChId, Enet_Handle hEnet, uint32_t coreKey, uint32_t coreId, EnetDma_PktQ *fqPktInfoQ, EnetDma_PktQ *cqPktInfoQ);",
    "EnetApp_coreAttach": "void NAME(Enet_Type enetType, uint32_t instId, uint32_t coreId, EnetPer_AttachCoreOutArgs *attachInfo);",
    "EnetApp_coreDetach": "void NAME(Enet_Type enetType, uint32_t instId, uint32_t coreId, uint32_t coreKey);",
    "EnetApp_getEnetInstInfo": "void NAME(uint32_t enetInstanceId, Enet_Type *enetType, uint32_t *instId);",
    "EnetApp_getEnetInstMacInfo": "void NAME(Enet_Type enetType, uint32_t instId, Enet_MacPort macPortList[], uint8_t *numMacPorts);",
    "EnetApp_getMacAddress": "void NAME(uint32_t enetRxDmaChId, EnetApp_GetMacAddrOutArgs *outArgs);",
    "EnetApp_getRxDmaHandle": "void NAME(uint32_t enetRxDmaChId, const EnetApp_GetDmaHandleInArgs *inArgs, EnetApp_GetRxDmaHandleOutArgs *outArgs);",
    "EnetApp_getTxDmaHandle": "void NAME(uint32_t enetTxDmaChId, const EnetApp_GetDmaHandleInArgs *inArgs, EnetApp_GetTxDmaHandleOutArgs *outArgs);",
    "EnetApp_isPortLinked": "bool NAME(Enet_Handle hEnet);",
    "EnetApp_releaseHandleInfo": "void NAME(Enet_Type enetType, uint32_t instId);",
    "EnetBoard_getMacAddrList": "void NAME(uint8_t macAddr[][ENET_MAC_ADDR_LEN], uint32_t maxMacEntries, uint32_t *pAvailMacEntries);",
    "EnetBoard_getPhyCfg": "const EnetBoard_PhyCfg *NAME(const EnetBoard_EthPort *ethPort);",
    "EnetBoard_setupPorts": "int32_t NAME(EnetBoard_EthPort *ethPorts, uint32_t numEthPorts);",
    "EnetSoc_deinit": "void NAME(void);",
    "EnetSoc_getClkFreq": "uint32_t NAME(Enet_Type enetType, uint32_t instId, uint32_t clkId);",
    "EnetSoc_getCoreId": "uint32_t NAME(void);",
    "EnetSoc_getDmaHandle": "EnetDma_Handle NAME(Enet_Type enetType, uint32_t instId);",
    "EnetSoc_getEFusedMacAddrs": "int32_t NAME(uint8_t macAddr[][ENET_MAC_ADDR_LEN], uint32_t *num);",
    "EnetSoc_getEnetHandle": "Enet_Handle NAME(Enet_Type enetType, uint32_t instId);",
    "EnetSoc_getEnetHandleByIdx": "Enet_Handle NAME(uint32_t idx);",
    "EnetSoc_getEnetNum": "uint32_t NAME(void);",
    "EnetSoc_getIntrTriggerType": "uint32_t NAME(Enet_Type enetType, uint32_t instId, uint32_t intrId);",
    "EnetSoc_getMacPortCaps": "uint32_t NAME(Enet_Type enetType, uint32_t instId, Enet_MacPort macPort);",
    "EnetSoc_getMacPortMax": "uint32_t NAME(Enet_Type enetType, uint32_t instId);",
    "EnetSoc_getMacPortMii": "int32_t NAME(Enet_Type enetType, uint32_t instId, Enet_MacPort macPort, EnetMacPort_Interface *mii);",
    "EnetSoc_init": "int32_t NAME(void);",
    "EnetSoc_isIpSupported": "uint32_t NAME(Enet_Type enetType, uint32_t instId);",
};

let gRenameRegex = null;

function getModuleInstances(moduleName)
{
    let module = system.modules[moduleName];
    if (module === undefined)
    {
        return [];
    }
    return module.$instances;
}

function isComboMode()
{
    return (supportedSocs.includes(common.getSocName()) &&
            (getModuleInstances(icssModuleName).length > 0) &&
            (getModuleInstances(cpswModuleName).length > 0));
}

function isCpswModule(moduleName)
{
    return (moduleName === cpswModuleName);
}

function getNamespaceSuffix(moduleName, identifier)
{
    let isMacro = /^[A-Z0-9_]+$/.test(identifier);
    if (isCpswModule(moduleName))
    {
        return isMacro ? "_CPSW" : "_Cpsw";
    }
    return isMacro ? "_ICSSG" : "_Icssg";
}

function getNamespacedName(moduleName, identifier)
{
    return identifier + getNamespaceSuffix(moduleName, identifier);
}

function namespaceText(moduleName, text)
{
    if (gRenameRegex === null)
    {
        gRenameRegex = new RegExp("\\b(" + gNamespacedIdentifiers.join("|") + ")\\b", "g");
    }
    return text.replace(gRenameRegex, (identifier) => getNamespacedName(moduleName, identifier));
}

/*
 *  ======== addLibraryPrototypes ========
 *  Declare the renamed copies of library-declared functions that 'text' uses
 *  but does not declare itself. The prototypes go right after the last
 *  #include, so the types they use are known.
 */
function addLibraryPrototypes(moduleName, text)
{
    let prototypes = [];

    for (let name in gLibraryPrototypes)
    {
        let renamed = getNamespacedName(moduleName, name);
        let used = new RegExp("\\b" + renamed + "\\b").test(text);
        /* Definitions and prototypes in generated code start at column 0,
         * calls are indented */
        let declared = new RegExp("^[A-Za-z_][^;=\\n]*\\b" + renamed + "\\s*\\(", "m").test(text);
        if (used && !declared)
        {
            prototypes.push(gLibraryPrototypes[name].replace("NAME", renamed));
        }
    }
    if (prototypes.length === 0)
    {
        return text;
    }

    let block = "\n/* Renamed copies of Enet LLD callbacks (CPSW + ICSSG on one core) */\n" +
                prototypes.join("\n") + "\n";
    let includes = [...text.matchAll(/^#include[^\n]*\n/gm)];
    if (includes.length === 0)
    {
        return block + text;
    }
    let last = includes[includes.length - 1];
    let pos = last.index + last[0].length;
    return text.slice(0, pos) + block + text.slice(pos);
}

function renderNamespaced(moduleName, text)
{
    return addLibraryPrototypes(moduleName, namespaceText(moduleName, text));
}

/*
 *  ======== renderSubTemplate ========
 *  Render one module's contribution to a common networking template.
 *  Outside combined mode this is exactly system.getTemplate(path)(moduleName).
 */
function renderSubTemplate(subTemplate, key)
{
    let moduleName = subTemplate.moduleName;
    let owner = gSingleInstanceSubTemplates[key];

    if (!isComboMode())
    {
        return system.getTemplate(subTemplate[key])(moduleName);
    }
    if (owner !== undefined)
    {
        return (owner === moduleName) ? system.getTemplate(subTemplate[key])(moduleName) : "";
    }
    return renderNamespaced(moduleName, system.getTemplate(subTemplate[key])(moduleName));
}

/*
 *  ======== renderBoardTemplate ========
 *  Render a module's ethphy board configuration (ti_board_config.c/h).
 *  Outside combined mode this is exactly system.getTemplate(path)(moduleName).
 */
function renderBoardTemplate(moduleName, templatePath)
{
    /* The wrapper template adds a line break after this text */
    let text = system.getTemplate(templatePath)(moduleName).replace(/\n$/, "");

    if (!isComboMode())
    {
        return text;
    }
    return renderNamespaced(moduleName, text);
}

/*
 *  ======== isLinkedToModule ========
 *  In combined mode each module's board configuration only describes the
 *  ETHPHYs linked to that module's instances.
 */
function isLinkedToModule(ethphyInstance, moduleName)
{
    if (!isComboMode())
    {
        return true;
    }
    return ethphyInstance.$sharedBy.some((user) => (user.$module.$name === moduleName));
}

/*
 *  ======== includeGuardBegin / includeGuardEnd ========
 *  Generated headers get include guards in combined mode, where both
 *  modules' sources include them in the same file.
 */
function includeGuardBegin(guard)
{
    return isComboMode() ? `#ifndef ${guard}\n#define ${guard}\n` : "";
}

function includeGuardEnd(guard)
{
    return isComboMode() ? `#endif /* ${guard} */\n` : "";
}

/*
 *  ======== renderComboTemplate ========
 *  Render the dispatch layer for one generated file, only in combined mode.
 */
function renderComboTemplate(name)
{
    if (!isComboMode())
    {
        return "";
    }
    return system.getTemplate(`/networking/common/templates/combo/${name}.xdt`)();
}

function sumOverModule(moduleName, fxn)
{
    let module = system.modules[moduleName];
    let total = 0;
    for (let inst of getModuleInstances(moduleName))
    {
        total += Number(fxn(module, inst));
    }
    return total;
}

/* Number of Enet instances / DMA channels owned by the ICSSG module: offset
 * of the CPSW entries in the global index spaces */
function getIcssgInstanceCount()
{
    return getModuleInstances(icssModuleName).length;
}

function getIcssgTxChannelCount()
{
    return sumOverModule(icssModuleName, (m, i) => m.getTxChannelCount(i));
}

function getIcssgRxChannelCount()
{
    return sumOverModule(icssModuleName, (m, i) => m.getRxChannelCount(i));
}

function getCpswTxChannelCount()
{
    return sumOverModule(cpswModuleName, (m, i) => m.getTxChannelCount(i));
}

function getCpswRxChannelCount()
{
    return sumOverModule(cpswModuleName, (m, i) => m.getRxChannelCount(i));
}

/* Offsets of the CPSW entries in the global instance / DMA channel index
 * spaces: zero outside combined mode */
function getCpswInstanceOffset()
{
    return isComboMode() ? getIcssgInstanceCount() : 0;
}

function getCpswTxChannelOffset()
{
    return isComboMode() ? getIcssgTxChannelCount() : 0;
}

function getCpswRxChannelOffset()
{
    return isComboMode() ? getIcssgRxChannelCount() : 0;
}

/* Instances whose packets come from the pools generated by the ICSSG module */
function getPktPoolInstances()
{
    if (!isComboMode())
    {
        return getModuleInstances(icssModuleName);
    }
    return getAllInstances().filter((inst) => inst.PktPoolEnable);
}

/* Packet pools are generated once (by the ICSSG module) for all instances
 * that enable them */
function isPktPoolEnabled(instance)
{
    if (!isComboMode())
    {
        return instance.PktPoolEnable;
    }
    return getAllInstances().some((inst) => inst.PktPoolEnable);
}

function isPktInfoOnlyEnabled(instance)
{
    if (!isComboMode())
    {
        return instance.PktInfoOnlyEnable;
    }
    return getAllInstances().some((inst) => inst.PktInfoOnlyEnable);
}

/* Instances whose packet-info-only memory comes from the ICSSG module */
function getPktInfoInstances()
{
    return isComboMode() ? getAllInstances() : getModuleInstances(icssModuleName);
}

function getAllInstances()
{
    return getModuleInstances(icssModuleName).concat(getModuleInstances(cpswModuleName));
}

/* lwIP netifs */
function getModuleNetifCount(moduleName)
{
    return sumOverModule(moduleName, (m, i) => m.getNetifCount(i));
}

function getCpswNetifOffset()
{
    return isComboMode() ? getModuleNetifCount(icssModuleName) : 0;
}

/* Default netif over both modules: { moduleName, name } or null */
function getDefaultNetif()
{
    for (let moduleName of [icssModuleName, cpswModuleName])
    {
        let module = system.modules[moduleName];
        for (let inst of getModuleInstances(moduleName))
        {
            for (let i = 0; i < module.getNetifCount(inst); i++)
            {
                if (module.getNetifConfig(inst, i).isDefault === true)
                {
                    return { moduleName: moduleName, name: module.getNetifConfig(inst, i).$name };
                }
            }
        }
    }
    return null;
}

/*
 *  ======== getModuleDefaultNetifIdx ========
 *  Value of a module's ENET_SYSCFG_DEFAULT_NETIF_IDX, which the module's
 *  lwipif code compares with its own (module-local) netif index. In combined
 *  mode only the module that owns the default netif has a valid index.
 */
function getModuleDefaultNetifIdx(moduleName, defaultNetifName)
{
    if (!isComboMode())
    {
        return defaultNetifName;
    }
    let dflt = getDefaultNetif();
    if ((dflt === null) || (dflt.moduleName !== moduleName))
    {
        return "0xFFFFFFFFU";
    }
    let offset = isCpswModule(moduleName) ? getCpswNetifOffset() : 0;
    return (offset === 0) ? dflt.name.toUpperCase() :
                            `${dflt.name.toUpperCase()} - ${offset}U`;
}

function hasCustomBoard(moduleName)
{
    return getModuleInstances(moduleName).some((inst) => inst.customBoardEnable);
}

function isMcmEnabled(moduleName)
{
    let instances = getModuleInstances(moduleName);
    return (instances.length > 0) && instances[0].McmEnable;
}

/*
 *  ======== validate ========
 *  Checks for combined mode, called from both modules' validate().
 */
function validate(instance, report)
{
    if (!isComboMode())
    {
        return;
    }

    /* One driver model for both peripherals */
    if (isCpswModule(instance.$module.$name))
    {
        let icssInst = getModuleInstances(icssModuleName)[0];
        if (instance.RtosVariant !== icssInst.RtosVariant)
        {
            report.logError(`Must match the Enet (ICSS) setting when CPSW and ICSSG ` +
                            `are used together on one core`, instance, "RtosVariant");
        }
    }

    /* enet_mcm.c keeps one MCM per peripheral family, and an MCM serves one
     * Enet instance */
    let moduleInstances = getModuleInstances(instance.$module.$name);
    if (instance.McmEnable && (moduleInstances.length > 1) && (instance === moduleInstances[0]))
    {
        report.logWarning("With CPSW and ICSSG on one core, MCM serves one Enet instance per " +
                          "peripheral: only the first instance opened is managed by MCM",
                          instance, "McmEnable");
    }

    let module = system.modules[instance.$module.$name];
    if (Number(module.getNetifCount(instance)) > 0)
    {
        if (instance.RtosVariant !== "FreeRTOS")
        {
            report.logError("LwIP netifs need FreeRTOS when CPSW and ICSSG are used together " +
                            "on one core", instance, "RtosVariant");
        }

        let numDefault = 0;
        for (let moduleName of [icssModuleName, cpswModuleName])
        {
            let m = system.modules[moduleName];
            for (let inst of getModuleInstances(moduleName))
            {
                for (let i = 0; i < m.getNetifCount(inst); i++)
                {
                    numDefault += (m.getNetifConfig(inst, i).isDefault === true) ? 1 : 0;
                }
            }
        }
        if (numDefault > 1)
        {
            report.logError("Only one netif of Enet (ICSS) and Enet (CPSW) can be the default netif " +
                            "when CPSW and ICSSG are used together on one core", instance, "netifInstance");
        }
    }
}

exports = {
    icssModuleName,
    cpswModuleName,
    isComboMode,
    isCpswModule,
    getNamespacedName,
    namespaceText,
    renderSubTemplate,
    renderBoardTemplate,
    renderComboTemplate,
    isLinkedToModule,
    includeGuardBegin,
    includeGuardEnd,
    getCpswInstanceOffset,
    getCpswTxChannelOffset,
    getCpswRxChannelOffset,
    getPktPoolInstances,
    validate,
    getModuleInstances,
    getIcssgInstanceCount,
    getIcssgTxChannelCount,
    getIcssgRxChannelCount,
    getCpswTxChannelCount,
    getCpswRxChannelCount,
    getAllInstances,
    getPktInfoInstances,
    isPktPoolEnabled,
    isPktInfoOnlyEnabled,
    getModuleNetifCount,
    getCpswNetifOffset,
    getDefaultNetif,
    getModuleDefaultNetifIdx,
    hasCustomBoard,
    isMcmEnabled,
};
