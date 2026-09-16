/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - vru_selftest.c                                          *
 *                                                                         *
 *   Proves the Voice Recognition Unit's recognizer without a game, a       *
 *   frontend or a microphone: it loads the same library and model the core *
 *   loads, sends the word list the way a game sends it, pushes a recorded  *
 *   utterance through the same entry point the microphone feeds, and       *
 *   checks the slot that comes back.                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "plugin/vru_recognizer.h"
#include "plugin/vru_word_table.h"

#include <libretro.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The five plugin entry points, declared the way plugin.c declares them --
 * they are wired into the core's function table, not into a header. */
extern void vru_SendVRUWord(uint16_t length, uint16_t* word, uint8_t lang);
extern void vru_SetMicState(int state);
extern void vru_ReadVRUResults(uint16_t* error_flags, uint16_t* num_results,
                               uint16_t* mic_level, uint16_t* voice_level,
                               uint16_t* voice_length, uint16_t* matches);
extern void vru_ClearVRUWords(uint8_t length);
extern void vru_SetVRUWordMask(uint8_t length, uint8_t* mask);

/* What the core would supply. The recognizer asks the frontend for a
 * microphone only in the path this test does not take. */
retro_environment_t environ_cb = NULL;
int pad_types[4] = {0, 0, 0, 0};

void DebugMessage(int level, const char* message, ...)
{
    va_list args;
    va_start(args, message);
    fprintf(stderr, "[m64p:%d] ", level);
    vfprintf(stderr, message, args);
    fputs("\n", stderr);
    va_end(args);
}

static int g_failures = 0;

static void ok(int cond, const char* name, const char* detail)
{
    if (cond)
    {
        printf("[vru-selftest] PASS  %s\n", name);
    }
    else
    {
        g_failures++;
        printf("[vru-selftest] FAIL  %s%s%s\n", name,
               (detail != NULL) ? "  -- " : "", (detail != NULL) ? detail : "");
    }
}

/* ---------------------------------------------------------------- */

static int table_compare(const void* key, const void* row)
{
    return strcmp((const char*)key, ((const struct vru_word_entry*)row)->hex);
}

static void test_word_table(void)
{
    const struct vru_word_entry* found;
    int sorted = 1;
    int i;

    for (i = 1; i < VRU_WORD_TABLE_COUNT; i++)
    {
        if (strcmp(g_vru_word_table[i - 1].hex, g_vru_word_table[i].hex) >= 0)
            sorted = 0;
    }
    ok(sorted, "table/sorted, so it can be searched", NULL);
    ok(VRU_WORD_TABLE_COUNT > 600, "table/carries both games' vocabularies", NULL);

    found = (const struct vru_word_entry*)bsearch(
        "03CF00A80003036000EA", g_vru_word_table, VRU_WORD_TABLE_COUNT,
        sizeof(g_vru_word_table[0]), table_compare);
    ok(found != NULL && strcmp(found->text, "pikachu") == 0,
       "table/the word a game sends resolves to text",
       (found != NULL) ? found->text : "not found");
}

/* ---------------------------------------------------------------- */

static int16_t* read_wav(const char* path, unsigned* out_count, unsigned* out_rate)
{
    unsigned char header[12];
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    if (fread(header, 1, 12, f) != 12 || memcmp(header, "RIFF", 4) != 0
        || memcmp(header + 8, "WAVE", 4) != 0)
    {
        fclose(f);
        return NULL;
    }

    *out_rate = 0;
    for (;;)
    {
        unsigned char chunk[8];
        unsigned size;
        if (fread(chunk, 1, 8, f) != 8)
            break;
        size = (unsigned)chunk[4] | ((unsigned)chunk[5] << 8)
             | ((unsigned)chunk[6] << 16) | ((unsigned)chunk[7] << 24);

        if (memcmp(chunk, "fmt ", 4) == 0)
        {
            unsigned char fmt[16];
            if (fread(fmt, 1, (size < 16) ? size : 16, f) != ((size < 16) ? size : 16))
                break;
            *out_rate = (unsigned)fmt[4] | ((unsigned)fmt[5] << 8)
                      | ((unsigned)fmt[6] << 16) | ((unsigned)fmt[7] << 24);
            if (size > 16)
                fseek(f, (long)(size - 16), SEEK_CUR);
        }
        else if (memcmp(chunk, "data", 4) == 0)
        {
            int16_t* samples = (int16_t*)malloc(size);
            if (samples == NULL)
                break;
            if (fread(samples, 1, size, f) != size)
            {
                free(samples);
                break;
            }
            fclose(f);
            *out_count = size / 2;
            return samples;
        }
        else
        {
            fseek(f, (long)(size + (size & 1)), SEEK_CUR);
        }
    }

    fclose(f);
    return NULL;
}

/* A word as the game sends it: big-endian phoneme codes, which the table is
 * keyed by. */
static uint16_t k_pikachu[] = {0x03CF, 0x00A8, 0x0003, 0x0360, 0x00EA};
static uint16_t k_hey[]     = {0x0408, 0x0174, 0x0024};

static void test_recognizer(const char* system_dir, const char* wav_path, int expect_match)
{
    uint16_t error_flags = 0, num_results = 0, mic_level = 0;
    uint16_t voice_level = 0, voice_length = 0, matches[10];
    unsigned count = 0, rate = 0;
    int16_t* samples;
    char detail[128];
    unsigned pushed = 0;

    if (getenv("VRU_VERBOSE") != NULL)
        vru_recognizer_set_options("auto", "push-to-talk", 1.0f, 1);

    if (!vru_recognizer_start_offline(system_dir))
    {
        printf("[vru-selftest] SKIP  no speech library or model under %s/vru\n", system_dir);
        printf("[vru-selftest] SKIP  (this is the degradation the core ships with)\n");
        return;
    }
    ok(vru_recognizer_is_available(), "engine/library and worker came up", NULL);

    samples = read_wav(wav_path, &count, &rate);
    if (samples == NULL)
    {
        ok(0, "engine/the utterance was readable", wav_path);
        return;
    }
    sprintf(detail, "%u samples at %u Hz", count, rate);
    ok(count > 0, "engine/the utterance was readable", detail);

    /* The list the game loads, in the order it loads it: the slot is the
     * position, and the slot is what comes back. */
    vru_ClearVRUWords(2);
    vru_SendVRUWord(sizeof(k_pikachu) / 2, k_pikachu, 0);
    vru_SendVRUWord(sizeof(k_hey) / 2, k_hey, 0);

    /* Z down, speak, Z up -- the boundary a game times an utterance by. */
    vru_recognizer_talking(0, 1);
    while (pushed < count)
    {
        unsigned block = (count - pushed > 2048u) ? 2048u : (count - pushed);
        vru_recognizer_push_pcm(samples + pushed, block);
        pushed += block;
    }
    vru_recognizer_talking(0, 0);

    vru_ReadVRUResults(&error_flags, &num_results, &mic_level, &voice_level,
                       &voice_length, matches);

    sprintf(detail, "num_results=%u matches[0]=%u error=0x%04X",
            num_results, matches[0], error_flags);
    if (expect_match)
    {
        /* error_flags matters as much as the slot: "heard something that
         * matched nothing" also reports slot 0, so a test that reads only
         * matches[0] passes when the recognizer understood nothing. */
        ok(num_results >= 1 && matches[0] == 0 && error_flags == 0,
           "engine/heard the first word in the list", detail);
        ok(mic_level == 0x0BB8 && voice_length == 0x8004,
           "engine/the reply carries the levels a game expects", NULL);
    }
    else
    {
        ok(error_flags == 0x4000,
           "engine/a word the game is not listening for is refused", detail);
    }

    free(samples);
    vru_recognizer_close();
}

int main(int argc, char** argv)
{
    const char* system_dir = (argc > 1) ? argv[1] : ".";
    const char* wav_path = (argc > 2) ? argv[2] : NULL;
    const char* control_path = (argc > 3) ? argv[3] : NULL;

    test_word_table();

    if (wav_path != NULL)
        test_recognizer(system_dir, wav_path, 1);
    else
        printf("[vru-selftest] SKIP  no utterance given"
               " (usage: vru_selftest <system dir> <wav> [control wav])\n");

    /* The half that proves the other half: something the game is not listening
     * for must come back refused, or the check above cannot fail. */
    if (control_path != NULL)
        test_recognizer(system_dir, control_path, 0);

    printf("[vru-selftest] ---- %s ----\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
