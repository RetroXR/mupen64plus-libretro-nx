/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - mbc3_rtc_selftest.c                                     *
 *                                                                         *
 *   Proves the clock of a Game Boy cartridge in a Transfer Pak without a   *
 *   core, a game or a frontend: the real mbc3_rtc.c, over a clock the test *
 *   moves by hand and a storage standing in for the kept file.             *
 *                                                                         *
 *   gcc -I mupen64plus-core/src -o mbc3_rtc_selftest                       *
 *       tools/mbc3_rtc_selftest.c mupen64plus-core/src/device/gb/mbc3_rtc.c *
 *   ./mbc3_rtc_selftest                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "device/gb/mbc3_rtc.h"
#include "backends/api/clock_backend.h"
#include "backends/api/storage_backend.h"

#include <stdio.h>
#include <string.h>

static time_t g_now;

static time_t fake_time(void* clock)
{
    (void)clock;
    return g_now;
}

static const struct clock_backend_interface fake_clock = { fake_time };

struct fake_storage
{
    uint8_t data[MBC3_RTC_SAVE_SIZE];
    int saves;
};

static uint8_t* fake_data(const void* storage)
{
    return ((struct fake_storage*)(void*)storage)->data;
}

static size_t fake_size(const void* storage)
{
    (void)storage;
    return MBC3_RTC_SAVE_SIZE;
}

static void fake_save(void* storage, size_t start, size_t size)
{
    (void)start;
    (void)size;
    ++((struct fake_storage*)storage)->saves;
}

static const struct storage_backend_interface fake_istorage = { fake_data, fake_size, fake_save };

static int g_failures;

static void check(int ok, const char* what)
{
    printf("[mbc3] %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

static void make_rtc(struct mbc3_rtc* rtc, struct fake_storage* storage)
{
    memset(rtc, 0, sizeof(*rtc));
    init_mbc3_rtc(rtc, NULL, &fake_clock);
    if (storage != NULL) {
        rtc->storage = storage;
        rtc->istorage = &fake_istorage;
    }
}

static void blank_storage(struct fake_storage* storage)
{
    memset(storage->data, 0xff, sizeof(storage->data));
    storage->saves = 0;
}

/* What a game reads: latch, read, unlatch. */
static unsigned int read_reg(struct mbc3_rtc* rtc, unsigned int reg)
{
    unsigned int value;
    latch_mbc3_rtc_regs(rtc, 0);
    latch_mbc3_rtc_regs(rtc, 1);
    value = read_mbc3_rtc_regs(rtc, reg);
    latch_mbc3_rtc_regs(rtc, 0);
    return value;
}

static unsigned int read_days(struct mbc3_rtc* rtc)
{
    return read_reg(rtc, MBC3_RTC_DAYS_L) | ((read_reg(rtc, MBC3_RTC_DAYS_H) & 0x01) << 8);
}

int main(void)
{
    struct mbc3_rtc rtc;
    struct mbc3_rtc again;
    struct fake_storage storage;

    /* A clock with nothing kept counts from power-on. It used to start from a
     * last_time of 0, which added the whole Unix time on the first read. */
    g_now = 1790000000;
    make_rtc(&rtc, NULL);
    poweron_mbc3_rtc(&rtc);
    g_now += 5;
    check(read_reg(&rtc, MBC3_RTC_SECONDS) == 5 && read_days(&rtc) == 0
            && !(read_reg(&rtc, MBC3_RTC_DAYS_H) & 0x80),
        "a fresh clock counts from power-on, not from 1970");

    /* Kept at once, so a clock nobody sets still counts across a power cycle. */
    blank_storage(&storage);
    make_rtc(&rtc, &storage);
    poweron_mbc3_rtc(&rtc);
    check(storage.saves == 1, "a fresh kept clock is written at power-on");
    g_now += 90;
    make_rtc(&again, &storage);
    poweron_mbc3_rtc(&again);
    check(read_reg(&again, MBC3_RTC_MINUTES) == 1 && read_reg(&again, MBC3_RTC_SECONDS) == 30,
        "and counted while the machine was off");

    /* A clock the game sets survives a power cycle, counting while off. */
    blank_storage(&storage);
    make_rtc(&rtc, &storage);
    poweron_mbc3_rtc(&rtc);
    write_mbc3_rtc_regs(&rtc, MBC3_RTC_DAYS_L, 100);
    g_now += 5 * 3600;
    make_rtc(&again, &storage);
    poweron_mbc3_rtc(&again);
    check(read_days(&again) == 100 && read_reg(&again, MBC3_RTC_HOURS) == 5,
        "a set clock comes back after a power cycle, five hours on");
    poweron_mbc3_rtc(&again);
    check(read_days(&again) == 100, "and a second power-on does not clear it");

    /* The kept bytes are the shared layout: days low at 12, the time at 40. */
    {
        uint64_t t = 0;
        unsigned int k;
        for (k = 0; k < 8; ++k) {
            t |= (uint64_t)storage.data[40 + k] << (8 * k);
        }
        check(storage.data[12] == 100 && storage.data[13] == 0 && t == (uint64_t)rtc.last_time,
            "stored as uint32 counters and a uint64 host time");
    }

    /* A halted clock does not count. */
    blank_storage(&storage);
    make_rtc(&rtc, &storage);
    poweron_mbc3_rtc(&rtc);
    write_mbc3_rtc_regs(&rtc, MBC3_RTC_DAYS_H, 0x40);
    g_now += 1000;
    check(read_reg(&rtc, MBC3_RTC_SECONDS) == 0, "a halted clock stands still");
    write_mbc3_rtc_regs(&rtc, MBC3_RTC_DAYS_H, 0x00);
    g_now += 3;
    check(read_reg(&rtc, MBC3_RTC_SECONDS) == 3, "and counts again once released");

    /* A write keeps the time since the last update out of the new value. */
    blank_storage(&storage);
    make_rtc(&rtc, &storage);
    poweron_mbc3_rtc(&rtc);
    g_now += 50;
    write_mbc3_rtc_regs(&rtc, MBC3_RTC_SECONDS, 0);
    g_now += 2;
    check(read_reg(&rtc, MBC3_RTC_SECONDS) == 2, "a written counter starts from the value written");

    /* The day counter is nine bits. */
    blank_storage(&storage);
    make_rtc(&rtc, &storage);
    poweron_mbc3_rtc(&rtc);
    write_mbc3_rtc_regs(&rtc, MBC3_RTC_DAYS_L, 255);
    g_now += 86400;
    check(read_days(&rtc) == 256, "the day counter passes 255");

    blank_storage(&storage);
    make_rtc(&rtc, &storage);
    poweron_mbc3_rtc(&rtc);
    write_mbc3_rtc_regs(&rtc, MBC3_RTC_DAYS_L, 255);
    write_mbc3_rtc_regs(&rtc, MBC3_RTC_HOURS, 23);
    write_mbc3_rtc_regs(&rtc, MBC3_RTC_MINUTES, 59);
    write_mbc3_rtc_regs(&rtc, MBC3_RTC_SECONDS, 59);
    g_now += 1;
    check(read_days(&rtc) == 256 && read_reg(&rtc, MBC3_RTC_HOURS) == 0,
        "and midnight on day 255 carries into day 256");

    printf("[mbc3] %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
