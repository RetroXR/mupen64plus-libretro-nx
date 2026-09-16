/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - mbc3_rtc.c                                              *
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

#include "mbc3_rtc.h"
#include "backends/api/clock_backend.h"
#include "backends/api/storage_backend.h"
#include <string.h>

#define MBC3_RTC_HALT 0x40

static void update_rtc_regs(struct mbc3_rtc* rtc)
{
    /* compute elapsed time since last update */
    time_t now = rtc->iclock->get_time(rtc->clock);
    time_t diff = now - rtc->last_time;
    rtc->last_time = now;

    /* a halted clock does not count */
    if (rtc->regs[MBC3_RTC_DAYS_H] & MBC3_RTC_HALT) {
        return;
    }

    /* increment regs */
    if (diff > 0) {
        unsigned int day_carry = 0;

        rtc->regs[MBC3_RTC_SECONDS] += (int)(diff % 60);
        if (rtc->regs[MBC3_RTC_SECONDS] >= 60) {
            rtc->regs[MBC3_RTC_SECONDS] -= 60;
            ++rtc->regs[MBC3_RTC_MINUTES];
        }
        diff /= 60;

        rtc->regs[MBC3_RTC_MINUTES] += (int)(diff % 60);
        if (rtc->regs[MBC3_RTC_MINUTES] >= 60) {
            rtc->regs[MBC3_RTC_MINUTES] -= 60;
            ++rtc->regs[MBC3_RTC_HOURS];
        }
        diff /= 60;

        rtc->regs[MBC3_RTC_HOURS] += (int)(diff % 24);
        if (rtc->regs[MBC3_RTC_HOURS] >= 24) {
            rtc->regs[MBC3_RTC_HOURS] -= 24;
            day_carry = 1;
        }
        diff /= 24;

        /* update days counter, 9 bits across DAYS_L and bit 0 of DAYS_H */
        unsigned int days = (((rtc->regs[MBC3_RTC_DAYS_H] & 0x01) << 8) | rtc->regs[MBC3_RTC_DAYS_L])
            + (int)diff + day_carry;

        rtc->regs[MBC3_RTC_DAYS_L] = days & 0xff;
        rtc->regs[MBC3_RTC_DAYS_H] = (rtc->regs[MBC3_RTC_DAYS_H] & ~0x01) | ((days >> 8) & 0x01);

        /* set carry bit if days overflow */
        if (days > 511) { rtc->regs[MBC3_RTC_DAYS_H] |= 0x80; }
    }
}

static void save_mbc3_rtc(struct mbc3_rtc* rtc)
{
    uint8_t* data;
    uint64_t t = (uint64_t)(int64_t)rtc->last_time;
    unsigned int i, k;

    if (rtc->storage == NULL) {
        return;
    }

    data = rtc->istorage->data(rtc->storage);
    for (i = 0; i < MBC3_RTC_REGS_COUNT; ++i) {
        for (k = 0; k < 4; ++k) {
            data[4 * i + k] = (k == 0) ? rtc->regs[i] : 0;
            data[20 + 4 * i + k] = (k == 0) ? rtc->latched_regs[i] : 0;
        }
    }
    for (k = 0; k < 8; ++k) {
        data[40 + k] = (uint8_t)(t >> (8 * k));
    }
    rtc->istorage->save(rtc->storage, 0, MBC3_RTC_SAVE_SIZE);
}

static int load_mbc3_rtc(struct mbc3_rtc* rtc)
{
    const uint8_t* data;
    uint64_t t = 0;
    unsigned int i, k;

    if (rtc->storage == NULL || rtc->istorage->size(rtc->storage) < MBC3_RTC_SAVE_SIZE) {
        return 0;
    }

    data = rtc->istorage->data(rtc->storage);
    for (k = 0; k < 8; ++k) {
        t |= (uint64_t)data[40 + k] << (8 * k);
    }
    if (t == UINT64_MAX) {
        return 0;
    }

    for (i = 0; i < MBC3_RTC_REGS_COUNT; ++i) {
        rtc->regs[i] = data[4 * i];
        rtc->latched_regs[i] = data[20 + 4 * i];
    }
    rtc->last_time = (time_t)(int64_t)t;
    return 1;
}

void init_mbc3_rtc(struct mbc3_rtc* rtc,
                   void* clock,
                   const struct clock_backend_interface* iclock)
{
    rtc->clock = clock;
    rtc->iclock = iclock;
}

/* The cartridge's battery keeps its clock through a power cycle, so power-on
 * restores a kept clock rather than clearing it. A clock with nothing kept starts
 * at zero NOW, and is written at once so it goes on counting from here. */
void poweron_mbc3_rtc(struct mbc3_rtc* rtc)
{
    rtc->latch = 0;
    if (load_mbc3_rtc(rtc)) {
        return;
    }

    memset(rtc->regs, 0, MBC3_RTC_REGS_COUNT);
    memset(rtc->latched_regs, 0, MBC3_RTC_REGS_COUNT);
    rtc->last_time = rtc->iclock->get_time(rtc->clock);
    save_mbc3_rtc(rtc);
}

uint8_t read_mbc3_rtc_regs(struct mbc3_rtc* rtc, unsigned int reg)
{
    if (rtc->latch) {
        return rtc->latched_regs[reg];
    }

    update_rtc_regs(rtc);
    return rtc->regs[reg];
}

void write_mbc3_rtc_regs(struct mbc3_rtc* rtc, unsigned int reg, uint8_t value)
{
    /* the time since the last update belongs to the old value */
    update_rtc_regs(rtc);
    rtc->regs[reg] = value;
    save_mbc3_rtc(rtc);
}

void latch_mbc3_rtc_regs(struct mbc3_rtc* rtc, uint8_t data)
{
    if (rtc->latch == 0 && data == 1) {
        update_rtc_regs(rtc);
        memcpy(rtc->latched_regs, rtc->regs, MBC3_RTC_REGS_COUNT);
    }

    rtc->latch = data & 0x1;
}
