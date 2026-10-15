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
 * \file  enet_ioctl_register_multi.c
 *
 * \brief IOCTL handler registration for libraries that include both CPSW and
 *        ICSSG. See priv/core/enet_ioctl_register_multi_priv.h.
 */

/* ========================================================================== */
/*                             Include Files                                  */
/* ========================================================================== */

#include <enet_cfg.h>
#include <enet.h>
#include <include/core/enet_per.h>
#include <priv/core/enet_ioctl_register_multi_priv.h>

#if defined(ENET_IOCTL_REGISTER_MULTI_PER)

/* ========================================================================== */
/*                           Macros & Typedefs                                */
/* ========================================================================== */

/* Enet_ioctl_register_<cmd>() for an IOCTL that both peripherals implement */
#define ENET_IOCTL_REGISTER_MULTI_PER_FXN(cmd)                                          \
    extern int32_t Cpsw_ioctl_register_##cmd(Enet_Handle hEnet, uint32_t coreId);       \
    extern int32_t Icssg_ioctl_register_##cmd(Enet_Handle hEnet, uint32_t coreId);      \
    int32_t Enet_ioctl_register_##cmd(Enet_Handle hEnet, uint32_t coreId);              \
    int32_t Enet_ioctl_register_##cmd(Enet_Handle hEnet, uint32_t coreId)               \
    {                                                                                   \
        int32_t status = ENET_EBADARGS;                                                 \
                                                                                        \
        if ((hEnet != NULL) && (hEnet->enetPer != NULL))                                \
        {                                                                               \
            if (Enet_isIcssFamily(hEnet->enetPer->enetType))                            \
            {                                                                           \
                status = Icssg_ioctl_register_##cmd(hEnet, coreId);                     \
            }                                                                           \
            else                                                                        \
            {                                                                           \
                status = Cpsw_ioctl_register_##cmd(hEnet, coreId);                      \
            }                                                                           \
        }                                                                               \
                                                                                        \
        return status;                                                                  \
    }

/* Enet_ioctl_register_<cmd>() for an IOCTL that only ICSSG implements in this
 * library configuration */
#define ENET_IOCTL_REGISTER_ICSSG_ONLY_FXN(cmd)                                         \
    extern int32_t Icssg_ioctl_register_##cmd(Enet_Handle hEnet, uint32_t coreId);      \
    int32_t Enet_ioctl_register_##cmd(Enet_Handle hEnet, uint32_t coreId);              \
    int32_t Enet_ioctl_register_##cmd(Enet_Handle hEnet, uint32_t coreId)               \
    {                                                                                   \
        int32_t status = ENET_EBADARGS;                                                 \
                                                                                        \
        if ((hEnet != NULL) && (hEnet->enetPer != NULL))                                \
        {                                                                               \
            if (Enet_isIcssFamily(hEnet->enetPer->enetType))                            \
            {                                                                           \
                status = Icssg_ioctl_register_##cmd(hEnet, coreId);                     \
            }                                                                           \
            else                                                                        \
            {                                                                           \
                status = ENET_ENOTSUPPORTED;                                            \
            }                                                                           \
        }                                                                               \
                                                                                        \
        return status;                                                                  \
    }

/* ========================================================================== */
/*                          Function Definitions                              */
/* ========================================================================== */

ENET_IOCTL_REGISTER_MULTI_PER_CMDS(ENET_IOCTL_REGISTER_MULTI_PER_FXN)

#if ENET_CFG_IS_ON(CPSW_IET_INCL)
ENET_IOCTL_REGISTER_MULTI_PER_IET_CMDS(ENET_IOCTL_REGISTER_MULTI_PER_FXN)
#else
ENET_IOCTL_REGISTER_MULTI_PER_IET_CMDS(ENET_IOCTL_REGISTER_ICSSG_ONLY_FXN)
#endif

#if ENET_CFG_IS_ON(CPSW_EST)
ENET_IOCTL_REGISTER_MULTI_PER_EST_CMDS(ENET_IOCTL_REGISTER_MULTI_PER_FXN)
#else
ENET_IOCTL_REGISTER_MULTI_PER_EST_CMDS(ENET_IOCTL_REGISTER_ICSSG_ONLY_FXN)
#endif

#endif /* ENET_IOCTL_REGISTER_MULTI_PER */
