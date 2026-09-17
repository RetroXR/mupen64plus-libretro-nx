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
#include <retro_timers.h>

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

/* Words as the Japanese titles send them: one Shift-JIS character to each
 * uint16. Measured from Pikachuu Genki de Chuu. */
static uint16_t k_ja_pikachu[]  = {0x82D2, 0x82A9, 0x82BF, 0x82E3, 0x82A4};   /* ぴかちゅう */
static uint16_t k_ja_hello[]    = {0x82B1, 0x82F1, 0x82C9, 0x82BF, 0x82ED};   /* こんにちわ */
static uint16_t k_ja_start[]    = {0x82B0, 0x815B, 0x82DE, 0x82B7, 0x82BD,
                                   0x815B, 0x82C6};                           /* げーむすたーと */
static uint16_t k_ja_row_ends[] = {0x829F, 0x82F1, 0x8340, 0x837E, 0x8380, 0x8396};
static uint16_t k_ja_kanji[]    = {0x82D2, 0x8ABF};                           /* ぴ漢 */
static uint16_t k_ascii[]       = {0x0041, 0x0042};

static int decodes_to(uint16_t* word, uint16_t length, const char* expected, char* got)
{
    unsigned refused = 1;
    int japanese = vru_recognizer_decode_word(word, length, got, 128, &refused);
    return japanese && refused == 0 && strcmp(got, expected) == 0;
}

static int folds_to(const char* text, int keep_spaces, const char* expected, char* got)
{
    return vru_recognizer_fold_kana(text, got, 128, keep_spaces) && strcmp(got, expected) == 0;
}

static void test_japanese_text(void)
{
    char got[128];
    unsigned refused;

    ok(decodes_to(k_ja_pikachu, 5, "ぴかちゅう", got),
       "japanese/a word the game sends reads as UTF-8", got);
    ok(decodes_to(k_ja_start, 7, "げーむすたーと", got),
       "japanese/and so does the long vowel", got);
    ok(decodes_to(k_ja_row_ends, 6, "ぁんァミムヶ", got),
       "japanese/both ends of each kana row, across the gap at 0x837F", got);

    refused = 0;
    ok(!vru_recognizer_decode_word(k_ja_kanji, 2, got, sizeof(got), &refused)
       && refused == 0x8ABF,
       "japanese/a character that is not kana refuses the word, and is named", NULL);
    refused = 1;
    ok(!vru_recognizer_decode_word(k_pikachu, 5, got, sizeof(got), &refused) && refused == 0,
       "japanese/a phoneme word the table lacks is not mistaken for Shift-JIS", NULL);
    ok(!vru_recognizer_decode_word(k_ascii, 2, got, sizeof(got), &refused),
       "japanese/nor is plain ASCII", NULL);
    ok(!vru_recognizer_decode_word(k_ja_start, 7, got, 8, &refused),
       "japanese/a word longer than its buffer is refused, not cut short", NULL);

    ok(folds_to("ぴかちゅう", 0, "ピカチュウ", got), "fold/hiragana is written as katakana", got);
    ok(folds_to("ゲームすたーと", 0, "ゲームスタート", got),
       "fold/katakana and the long vowel stay as they are", got);
    ok(folds_to("pikachu", 0, "pikachu", got), "fold/an English word is untouched", got);
    ok(folds_to("ぴか ちゅう　です", 0, "ピカチュウデス", got),
       "fold/spaces go, the full-width one too", got);
    ok(folds_to("ぴか ちゅう", 1, "ピカ チュウ", got), "fold/or stay as one space when asked", got);
}

static void test_segment(const char* system_dir)
{
    static const char* k_lexicon =
        "<eps> 0\n!SIL 1\n[unk] 2\nぴか 3\nピカ 4\nちゅう 5\nこんにちわ 6\nこん 7\n"
        "にち 8\nわ 9\nは 10\nハ 11\nい 12\n漢字 13\n";
    const char* path = "vru_selftest_words.txt";
    char phrase[300], real[1024];
    FILE* f = fopen(path, "wb");

    if (f == NULL)
    {
        ok(0, "segment/wrote a lexicon to split against", path);
        return;
    }
    fputs(k_lexicon, f);
    fclose(f);

    ok(vru_recognizer_segment(path, "こんにちわ", phrase, sizeof(phrase)) == 1
       && strcmp(phrase, "こんにちわ") == 0,
       "segment/a word the lexicon has whole goes whole, not as こん にち わ", phrase);
    ok(vru_recognizer_segment(path, "ぴかちゅう", phrase, sizeof(phrase)) == 1
       && strcmp(phrase, "ピカ ちゅう") == 0,
       "segment/one it lacks goes as the lexicon words that spell it", phrase);
    ok(vru_recognizer_segment(path, "はい", phrase, sizeof(phrase)) == 1
       && strcmp(phrase, "ハ い") == 0,
       "segment/the katakana spelling wins, since は can be the particle", phrase);
    ok(vru_recognizer_segment(path, "ぴかちゅ", phrase, sizeof(phrase)) == 0
       && phrase[0] == '\0',
       "segment/a word nothing spells is given as nothing", phrase);
    remove(path);
    ok(vru_recognizer_segment(path, "ぴか", phrase, sizeof(phrase)) == -1,
       "segment/a missing lexicon says so", NULL);

    /* The real lexicon, when the model is installed, and the misheard spelling
     * the Pikachu game sends beside the real one. */
    snprintf(real, sizeof(real), "%s/vru/model-ja/graph/words.txt", system_dir);
    f = fopen(real, "rb");
    if (f == NULL)
    {
        printf("[vru-selftest] SKIP  no model-ja under %s/vru\n", system_dir);
        return;
    }
    fclose(f);
    ok(vru_recognizer_segment(real, "ぴかっちゅう", phrase, sizeof(phrase)) == 1,
       "segment/model-ja spells ぴかっちゅう, which it does not have whole", phrase);
    printf("[vru-selftest]       as \"%s\"\n", phrase);
}

/* ---------------------------------------------------------------- */


struct sent_word
{
    uint16_t* codes;
    uint16_t  length;
};

/* Something said into the unit, and the slot that must come back for it, or
 * -1 for a word the game is not listening for. */
struct utterance
{
    const char* wav;
    int         slot;
    const char* what;
};

static const char* named(const char* group, const char* what)
{
    static char name[256];
    snprintf(name, sizeof(name), "%s/%s", group, what);
    return name;
}

static void test_recognizer(const char* group, const char* system_dir,
                            const struct sent_word* words, int word_count, uint8_t lang,
                            const struct utterance* said, int said_count)
{
    int i, u;

    if (getenv("VRU_VERBOSE") != NULL)
        vru_recognizer_set_options("auto", 1.0f, 1);

    if (!vru_recognizer_start_offline(system_dir))
    {
        printf("[vru-selftest] SKIP  no speech library or model under %s/vru\n", system_dir);
        printf("[vru-selftest] SKIP  (this is the degradation the core ships with)\n");
        return;
    }
    ok(vru_recognizer_is_available(), named(group, "library and worker came up"), NULL);

    /* The list the game loads, in the order it loads it: the slot is the
     * position, and the slot is what comes back. */
    vru_ClearVRUWords((uint8_t)word_count);
    for (i = 0; i < word_count; i++)
        vru_SendVRUWord(words[i].length, words[i].codes, lang);

    for (i = 0; i < 6000 && !vru_recognizer_grammar_ready(); i++)
        retro_sleep(10);
    ok(vru_recognizer_grammar_ready(), named(group, "the list was built into a grammar"), NULL);

    for (u = 0; u < said_count; u++)
    {
        /* The game's reply buffer is not clean: these start as the bytes a real
         * run left in the error word, so a field the core forgets to write
         * reads back as garbage rather than as a lucky zero. */
        uint16_t error_flags = 0xFE78, num_results = 0, mic_level = 0;
        uint16_t voice_level = 0, voice_length = 0, matches[10];
        unsigned count = 0, rate = 0, pushed = 0;
        int16_t* samples = read_wav(said[u].wav, &count, &rate);
        char detail[128];

        if (samples == NULL || count == 0)
        {
            ok(0, named(group, said[u].what), said[u].wav);
            free(samples);
            continue;
        }

        /* The player's own Z on the unit's port is not a talk button: nothing
         * but the game raises that line. */
        if (u == 0)
            ok(vru_recognizer_talking(3, 1) == 0,
               named(group, "the player's Z does not make the unit hear anyone"), NULL);

        /* The game says listen, the player speaks, the game says stop and asks
         * for the result at once, as Hey You, Pikachu! does around its own Z. */
        vru_SetMicState(1);
        if (u == 0)
            ok(vru_recognizer_talking(3, 0) == 1,
               named(group, "the unit hears someone while the game listens"), NULL);
        while (pushed < count)
        {
            unsigned block = (count - pushed > 2048u) ? 2048u : (count - pushed);
            vru_recognizer_push_pcm(samples + pushed, block);
            pushed += block;
        }
        vru_SetMicState(0);
        if (u == 0)
            ok(vru_recognizer_talking(3, 0) == 0,
               named(group, "and stops once the game does"), NULL);

        vru_ReadVRUResults(&error_flags, &num_results, &mic_level, &voice_level,
                           &voice_length, matches);

        sprintf(detail, "num_results=%u matches[0]=%u error=0x%04X",
                num_results, matches[0], error_flags);
        if (said[u].slot >= 0)
        {
            /* error_flags matters as much as the slot: "heard something that
             * matched nothing" also reports slot 0, so a test that reads only
             * matches[0] passes when the recognizer understood nothing. */
            ok(num_results >= 1 && matches[0] == said[u].slot && error_flags == 0,
               named(group, said[u].what), detail);
            ok(mic_level == 0x0BB8 && voice_length == 0x8004,
               named(group, "and the reply carries the levels a game expects"), NULL);
        }
        else
        {
            ok(error_flags == 0x4000, named(group, said[u].what), detail);
        }

        free(samples);
    }

    vru_recognizer_close();
}

int main(int argc, char** argv)
{
    static const struct sent_word english[] = {
        {k_pikachu, sizeof(k_pikachu) / 2}, {k_hey, sizeof(k_hey) / 2}
    };
    static const struct sent_word japanese[] = {
        {k_ja_start, sizeof(k_ja_start) / 2},
        {k_ja_pikachu, sizeof(k_ja_pikachu) / 2},
        {k_ja_hello, sizeof(k_ja_hello) / 2}
    };
    const char* positional[3] = {".", NULL, NULL};
    const char* ja_dir = NULL;
    struct utterance said[3];
    char ja_paths[3][1024];
    int i, at = 0, said_count = 0;

    for (i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "--ja-clips=", 11) == 0)
            ja_dir = argv[i] + 11;
        else if (at < 3)
            positional[at++] = argv[i];
    }

    test_word_table();
    test_japanese_text();
    test_segment(positional[0]);

    if (positional[1] != NULL)
    {
        said[said_count].wav = positional[1];
        said[said_count].slot = 0;
        said[said_count++].what = "heard the first word in the list";
    }
    /* The half that proves the other half: something the game is not listening
     * for must come back refused, or the check above cannot fail. */
    if (positional[2] != NULL)
    {
        said[said_count].wav = positional[2];
        said[said_count].slot = -1;
        said[said_count++].what = "a word the game is not listening for is refused";
    }
    if (said_count > 0)
        test_recognizer("engine", positional[0], english, 2, 0, said, said_count);
    else
        printf("[vru-selftest] SKIP  no utterance given (usage: vru_selftest <system dir>"
               " <wav> [control wav] [--ja-clips=<dir from vru_render_ja_clips.py>])\n");

    /* The same through model-ja, with the list sent as the Japanese titles send
     * it: through the decode, the lexicon and the folded match. */
    if (ja_dir != NULL)
    {
        snprintf(ja_paths[0], sizeof(ja_paths[0]), "%s/ja_pikachu.wav", ja_dir);
        snprintf(ja_paths[1], sizeof(ja_paths[1]), "%s/ja_game_start.wav", ja_dir);
        snprintf(ja_paths[2], sizeof(ja_paths[2]), "%s/ja_banana.wav", ja_dir);
        said[0].wav = ja_paths[0];
        said[0].slot = 1;
        said[0].what = "heard ぴかちゅう, a word model-ja has whole";
        said[1].wav = ja_paths[1];
        said[1].slot = 0;
        said[1].what = "heard げーむすたーと, which model-ja has only as ゲーム スタート";
        said[2].wav = ja_paths[2];
        said[2].slot = -1;
        said[2].what = "ばなな, which the game is not listening for, is refused";
        test_recognizer("engine-ja", positional[0], japanese, 3, 1, said, 3);
    }

    printf("[vru-selftest] ---- %s ----\n", (g_failures == 0) ? "PASS" : "FAIL");
    return (g_failures == 0) ? 0 : 1;
}
