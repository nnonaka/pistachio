/*********************************************************************
 *                
 * Copyright (C) 2002-2003, 2007,  Karlsruhe University
 *                
 * File path:     platform/pc99/rtc.h
 * Description:   driver for Real Time Clock
 *                
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *                
 * $Id: rtc.h,v 1.6 2003/09/24 19:04:59 skoglund Exp $
 *                
 ********************************************************************/
#ifndef __PLATFORM__PC99__RTC_H__
#define __PLATFORM__PC99__RTC_H__

#include INC_ARCH(ioport.h)	/* for in_u8/out_u8	*/


/**
 * Driver for Real Time Clock
 * @param base	the base address of the control registers
 *
 * The template parameter BASE enables compile-time resolution of the
 * RTC's control register addresses.
 *
 * Assumptions:
 * - BASE can be passed as port to in_u8/out_u8
 * - The RTC's data register is located at BASE+1
 *
 * Uses:
 * - out_u8, in_u8
 */


/**
 * Waits for a 1 second tick of the realtime clock.
 *
 * Written with direct port I/O (RTC index port 0x70, data port 0x71) rather
 * than the rtc_t<0x70> template so it is callable from both C and C++.
 */
INLINE void wait_for_second_tick(void)
{
    word_t reg;

    // wait that update bit is off
    do { out_u8(0x70, 0x0a); reg = in_u8(0x71); } while (reg & 0x80);

    // read second value
    out_u8(0x70, 0);
    word_t secstart = in_u8(0x71);

    // now wait until seconds change
    word_t sec;
    do { out_u8(0x70, 0); sec = in_u8(0x71); } while (secstart == sec);
}

#endif /* !__PLATFORM__PC99__RTC_H__ */
