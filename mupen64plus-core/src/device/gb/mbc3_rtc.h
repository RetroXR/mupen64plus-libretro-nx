/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - mbc3_rtc.h                                              *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2016 Bobby Smiles                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef M64P_DEVICE_GB_MBC3_RTC_H
#define M64P_DEVICE_GB_MBC3_RTC_H

#include <stdint.h>
#include <time.h>

struct clock_backend_interface;
struct storage_backend_interface;

/* The clock as its battery keeps it: little-endian uint32 seconds, minutes, hours,
 * days low and days high, the same five latched, then the uint64 host time the
 * counters were last brought up to date at. The layout mGBA, VBA-M and BGB use. A
 * time of all ones means no clock has been kept yet. */
#define MBC3_RTC_SAVE_SIZE 48

enum mbc3_rtc_registers
{
    MBC3_RTC_SECONDS,   /* 0-59 */
    MBC3_RTC_MINUTES,   /* 0-59 */
    MBC3_RTC_HOURS,     /* 0-23 */
    MBC3_RTC_DAYS_L,    /* Days Counter is 16 bit wide */
    MBC3_RTC_DAYS_H,    /* bits 0-8: days counter (0-511) */
                        /* bits 9-13: zeros ? */
                        /* bits 14: halt (0: stop timer, 1: active timer) */
                        /* bits 15: carry (0: no overflow, 1: couter overflowed) */
    MBC3_RTC_REGS_COUNT
};

struct mbc3_rtc
{
    uint8_t regs[MBC3_RTC_REGS_COUNT];

    unsigned int latch;
    uint8_t latched_regs[MBC3_RTC_REGS_COUNT];

    time_t last_time;

    void* clock;
    const struct clock_backend_interface* iclock;

    /* MBC3_RTC_SAVE_SIZE bytes the clock is kept in across power cycles, or NULL
     * for a clock that is not kept. */
    void* storage;
    const struct storage_backend_interface* istorage;
};

void init_mbc3_rtc(struct mbc3_rtc* rtc,
                   void* clock,
                   const struct clock_backend_interface* iclock);

void poweron_mbc3_rtc(struct mbc3_rtc* rtc);

uint8_t read_mbc3_rtc_regs(struct mbc3_rtc* rtc, unsigned int reg);
void write_mbc3_rtc_regs(struct mbc3_rtc* rtc, unsigned int reg, uint8_t value);
void latch_mbc3_rtc_regs(struct mbc3_rtc* rtc, uint8_t data);

#endif
