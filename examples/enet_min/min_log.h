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
 * min_log: tiny polled UART0 console for the lean enet_min examples.
 *
 * Provides the DPL log entry points (_DebugP_logZone, _DebugP_log) so that
 * DebugP_log() and library asserts print through a ~0.5 KB integer-only
 * formatter instead of the SDK printf, stdio and UART/UDMA drivers.
 * Supports %c %s %d %i %u %x %X %p %% with optional '0' flag and width, and
 * the 'l' length modifier (ignored, 32-bit). No floats, no 64-bit.
 */

#ifndef MIN_LOG_H_
#define MIN_LOG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UART0 functional clock and baud rate */
#ifndef MIN_LOG_UART_CLK_HZ
#define MIN_LOG_UART_CLK_HZ     (48000000U)
#endif
#ifndef MIN_LOG_BAUD
#define MIN_LOG_BAUD            (115200U)
#endif

/* Power up UART0, set its pins and program 8N1 at MIN_LOG_BAUD. Call once
 * after System_init(). Output before this call is dropped. */
void MinLog_init(void);

void MinLog_putc(char c);

#ifdef __cplusplus
}
#endif

#endif /* MIN_LOG_H_ */
