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


#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/dpl/DebugP.h>
#include <kernel/dpl/HwiP.h>
#include <kernel/dpl/SystemP.h>
#include <drivers/pinmux.h>
#include <drivers/sciclient.h>
#include <drivers/hw_include/cslr_soc.h>
#include "min_log.h"

#define REG32(a)        (*(volatile uint32_t *)(a))

/* 16550-style UART registers (32-bit stride) */
#define UART_THR        (0x00U)
#define UART_DLL        (0x00U)
#define UART_DLH        (0x04U)
#define UART_FCR        (0x08U)
#define UART_LCR        (0x0CU)
#define UART_LSR        (0x14U)
#define UART_MDR1       (0x20U)
#define UART_LSR_THRE   (0x20U)

static uint32_t gMinLogReady;

static const Pinmux_PerCfg_t gMinLogPins[] =
{
    { PIN_UART0_RXD, PIN_MODE(0) | PIN_INPUT_ENABLE },
    { PIN_UART0_TXD, PIN_MODE(0) },
    { PINMUX_END, 0U }
};

void MinLog_init(void)
{
    const uintptr_t base = CSL_UART0_BASE;
    const uint32_t  div  = (MIN_LOG_UART_CLK_HZ + (8U * MIN_LOG_BAUD)) / (16U * MIN_LOG_BAUD);

    (void)Sciclient_pmSetModuleState(TISCI_DEV_UART0, TISCI_MSG_VALUE_DEVICE_SW_STATE_ON,
                                     0U, SystemP_WAIT_FOREVER);
    Pinmux_config(gMinLogPins, PINMUX_DOMAIN_ID_MAIN);

    REG32(base + UART_MDR1) = 0x7U;                 /* disable */
    REG32(base + UART_LCR)  = 0x80U;                /* divisor latch */
    REG32(base + UART_DLL)  = div & 0xFFU;
    REG32(base + UART_DLH)  = (div >> 8) & 0x3FU;
    REG32(base + UART_LCR)  = 0x03U;                /* 8N1 */
    REG32(base + UART_FCR)  = 0x07U;                /* FIFO on, cleared */
    REG32(base + UART_MDR1) = 0x0U;                 /* UART 16x mode */
    gMinLogReady = 1U;
}

void MinLog_putc(char c)
{
    if (gMinLogReady != 0U)
    {
        while ((REG32(CSL_UART0_BASE + UART_LSR) & UART_LSR_THRE) == 0U)
        {
        }
        REG32(CSL_UART0_BASE + UART_THR) = (uint8_t)c;
    }
}

static void putStr(const char *s, uint32_t width, char pad)
{
    uint32_t n = 0U;

    while (s[n] != '\0')
    {
        n++;
    }
    for (; n < width; width--)
    {
        MinLog_putc(pad);
    }
    while (*s != '\0')
    {
        MinLog_putc(*s++);
    }
}

static void putNum(uint32_t v, uint32_t base, uint32_t neg, uint32_t width,
                   char pad, uint32_t upper)
{
    char     buf[12];
    uint32_t i = sizeof(buf) - 1U;
    const char *digits = (upper != 0U) ? "0123456789ABCDEF" : "0123456789abcdef";

    buf[i] = '\0';
    do
    {
        buf[--i] = digits[v % base];
        v /= base;
    } while (v != 0U);
    if (neg != 0U)
    {
        if (pad == '0')
        {
            MinLog_putc('-');
            width = (width > 0U) ? (width - 1U) : 0U;
        }
        else
        {
            buf[--i] = '-';
        }
    }
    putStr(&buf[i], width, pad);
}

static void minVprintf(const char *fmt, va_list va)
{
    while (*fmt != '\0')
    {
        char     pad   = ' ';
        uint32_t width = 0U;
        char     c     = *fmt++;

        if (c != '%')
        {
            if (c == '\n')
            {
                MinLog_putc('\r');
            }
            MinLog_putc(c);
            continue;
        }
        if (*fmt == '0')
        {
            pad = '0';
            fmt++;
        }
        while ((*fmt >= '0') && (*fmt <= '9'))
        {
            width = (width * 10U) + (uint32_t)(*fmt++ - '0');
        }
        while (*fmt == 'l')
        {
            fmt++;
        }
        c = *fmt;
        if (c == '\0')
        {
            break;
        }
        fmt++;
        switch (c)
        {
            case 'c':
                MinLog_putc((char)va_arg(va, int));
                break;
            case 's':
            {
                const char *s = va_arg(va, const char *);
                putStr((s != NULL) ? s : "(null)", width, ' ');
                break;
            }
            case 'd':
            case 'i':
            {
                int32_t v = va_arg(va, int32_t);
                putNum((v < 0) ? (0U - (uint32_t)v) : (uint32_t)v, 10U, (v < 0) ? 1U : 0U,
                       width, pad, 0U);
                break;
            }
            case 'u':
                putNum(va_arg(va, uint32_t), 10U, 0U, width, pad, 0U);
                break;
            case 'p':
                MinLog_putc('0');
                MinLog_putc('x');
                putNum(va_arg(va, uint32_t), 16U, 0U, 8U, '0', 0U);
                break;
            case 'x':
            case 'X':
                putNum(va_arg(va, uint32_t), 16U, 0U, width, pad, (c == 'X') ? 1U : 0U);
                break;
            default:
                MinLog_putc(c);
                break;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* DPL log entry points (replace kernel/nortos DebugP_nortos.c)              */
/* ------------------------------------------------------------------------ */

extern volatile uint32_t gDebugLogZone;
int32_t _DebugP_log(char *format, ...);
void DebugP_shmLogReaderTaskCreate(void);

void _DebugP_logZone(uint32_t logZone, char *format, ...)
{
    if ((HwiP_inISR() == 0U) && ((gDebugLogZone & logZone) == logZone))
    {
        va_list va;
        va_start(va, format);
        minVprintf(format, va);
        va_end(va);
    }
}

int32_t _DebugP_log(char *format, ...)
{
    if (HwiP_inISR() == 0U)
    {
        va_list va;
        va_start(va, format);
        minVprintf(format, va);
        va_end(va);
    }
    return 0;
}

void DebugP_shmLogReaderTaskCreate(void)
{
}
