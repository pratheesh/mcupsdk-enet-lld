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
 * cpsw_min_cpdma_regs.h: AM261x CPSW3G / CPDMA register map used by
 * cpsw_min_cpdma.c. Every offset is checked against the Enet LLD CSL headers
 * at build time by cpsw_min_cpdma_csl_check.c.
 */

#ifndef CPSW_MIN_CPDMA_REGS_H_
#define CPSW_MIN_CPDMA_REGS_H_

#define CPSW_BASE               (CSL_CPSW0_U_BASE)
#define CPSW_NU                 (CPSW_BASE + 0x20000U)      /* switch core */
#define CPDMA_BASE              (CPSW_NU + 0x14000U)
#define ALE_BASE                (CPSW_NU + 0x1E000U)

/* CPSW core */
#define CPSW_CTRL               (CPSW_NU + 0x0004U)
#define CPSW_P0_RX_MAXLEN       (CPSW_NU + 0x1024U)
#define CPSW_PN(p)              (CPSW_NU + 0x2000U + (((p) - 1U) * 0x1000U))
#define CPSW_PN_RX_MAXLEN(p)    (CPSW_PN(p) + 0x024U)
#define CPSW_PN_SA_L(p)         (CPSW_PN(p) + 0x308U)
#define CPSW_PN_SA_H(p)         (CPSW_PN(p) + 0x30CU)
#define CPSW_PN_MAC_CONTROL(p)  (CPSW_PN(p) + 0x330U)
#define CPSW_PN_MAC_STATUS(p)   (CPSW_PN(p) + 0x334U)
#define CPSW_PN_MAC_RESET(p)    (CPSW_PN(p) + 0x338U)

#define CONTROL_P0_ENABLE       (1U << 2)
#define CONTROL_P0_TX_CRC_REMOVE (1U << 13)
#define CONTROL_P0_RX_PAD       (1U << 14)
#define MACCTL_FULLDUPLEX       (1U << 0)
#define MACCTL_GMII_EN          (1U << 5)
#define MACCTL_CMD_IDLE         (1U << 11)
#define MACSTS_IDLE             (1U << 31)

/* ALE (same V5 block as AM243x) */
#define ALE_CTRL                (ALE_BASE + 0x008U)
#define ALE_PORTCTL(n)          (ALE_BASE + 0x040U + ((n) * 4U))
#define ALE_CTL_ENABLE          (1U << 31)
#define ALE_CTL_CLEAR           (1U << 30)
#define ALE_CTL_BYPASS          (1U << 4)
#define ALE_PORT_FORWARD        (3U)

/* CPDMA, channel 0 */
#define CPDMA_TX_CONTROL        (CPDMA_BASE + 0x004U)
#define CPDMA_RX_CONTROL        (CPDMA_BASE + 0x014U)
#define CPDMA_SOFT_RESET        (CPDMA_BASE + 0x01CU)
#define CPDMA_DMASTATUS         (CPDMA_BASE + 0x024U)
#define CPDMA_RX_BUF_OFFSET     (CPDMA_BASE + 0x028U)
#define CPDMA_TX_HDP            (CPDMA_BASE + 0x200U)
#define CPDMA_RX_HDP            (CPDMA_BASE + 0x220U)
#define DMASTATUS_HOST_ERR      (0x00F0F000U)   /* TX and RX host error codes */

/* MSS_CTRL: MAC port interface select (0 = MII, 1 = RMII, 2 = RGMII) */
#define MSS_CPSW_CONTROL        (CSL_MSS_CTRL_U_BASE + 0x860U)
#define MSS_PORT_MODE_MASK(p)   (0x7U << (((p) - 1U) * 16U))

/* CPPI 3.0 descriptor flags word */
#define DESC_SOP                (0x80000000U)
#define DESC_EOP                (0x40000000U)
#define DESC_OWNER              (0x20000000U)
#define DESC_EOQ                (0x10000000U)
#define DESC_TO_PORT_EN         (0x00100000U)   /* TX: directed to a port */
#define DESC_PORT_SHIFT         (16U)           /* TX to-port / RX from-port */
#define DESC_RX_FROM_PORT(f)    (((f) >> DESC_PORT_SHIFT) & 0x7U)
#define DESC_RX_ERR             (0x03700000U)   /* long, short, overrun, pkt error */
#define DESC_PKTLEN_MASK        (0x00000FFFU)

#endif /* CPSW_MIN_CPDMA_REGS_H_ */
