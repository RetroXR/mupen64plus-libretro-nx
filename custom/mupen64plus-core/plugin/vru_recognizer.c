/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - vru_recognizer.c                                        *
 *                                                                         *
 *   The speech recognizer behind the Voice Recognition Unit. The word      *
 *   table, the grammar, the match rule and the shape of the results are    *
 *   derived from Rosalie's Mupen GUI (Source/RMG-Input/VRU.cpp), which is  *
 *   GPLv3 -- so this file is GPLv3 or later.                              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Three threads meet here and each is allowed exactly one thing:
 *
 *   the frontend thread (retro_run)  owns every microphone call and the
 *                                    options, and pushes capture into the ring
 *   the emulation thread             calls the five plugin entry points and
 *                                    reads Z; touches flags under a lock and
 *                                    never speaks to Vosk
 *   the worker                       owns the model and the recognizer, and is
 *                                    the only thread that may block
 *
 * Vosk is opened at run time rather than linked, because this is the N64 core
 * everyone installs: a missing library must cost the VRU, not the core. */

#include "plugin/vru_recognizer.h"
#include "plugin/vru_word_table.h"

#include "api/m64p_types.h"
#include "api/m64p_plugin.h"
#include "api/callbacks.h"

#include <libretro.h>
#include <retro_timers.h>
#include <vosk_api.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* The VRU's own numbers, as the games expect them. Read from RMG; the core
 * writes them straight into the joybus reply. */
#define VRU_LEVEL_NOMINAL   0x0BB8
#define VRU_VOICE_LENGTH    0x8004
#define VRU_MATCH_EMPTY     0x7FFF
#define VRU_ERROR_DECODE    0x8000
#define VRU_ERROR_NO_MATCH  0x4000
#define VRU_MAX_MATCHES     5

/* ClearVRUWords takes a uint8 count, and a word is at most 40 uint16s of
 * joybus buffer: three UTF-8 bytes to a kana is 120. */
#define VRU_MAX_WORDS       256
#define VRU_WORD_CHARS      128
/* A word split into lexicon words, one space between each, with a space
 * either side to match on. */
#define VRU_PHRASE_CHARS    (VRU_WORD_CHARS * 2 + 3)
#define VRU_CAPTURE_RATE    48000
#define VRU_RING_SECONDS    8
/* Bounds the wait for an utterance's decode when the game asks for its result,
 * which is the moment the game stops listening. */
#define VRU_RESULT_WAIT_MS  3000

extern retro_environment_t environ_cb;
extern int pad_types[4];

typedef VoskModel*      (*ptr_vosk_model_new)(const char*);
typedef void            (*ptr_vosk_model_free)(VoskModel*);
typedef VoskRecognizer* (*ptr_vosk_recognizer_new)(VoskModel*, float);
typedef VoskRecognizer* (*ptr_vosk_recognizer_new_grm)(VoskModel*, float, const char*);
typedef void            (*ptr_vosk_recognizer_free)(VoskRecognizer*);
typedef void            (*ptr_vosk_recognizer_set_max_alternatives)(VoskRecognizer*, int);
typedef int             (*ptr_vosk_recognizer_accept_waveform_s)(VoskRecognizer*, const short*, int);
typedef const char*     (*ptr_vosk_recognizer_final_result)(VoskRecognizer*);
typedef void            (*ptr_vosk_recognizer_reset)(VoskRecognizer*);
typedef void            (*ptr_vosk_set_log_level)(int);

struct vru_vosk
{
    ptr_vosk_model_new                       model_new;
    ptr_vosk_model_free                      model_free;
    ptr_vosk_recognizer_new                  recognizer_new;
    ptr_vosk_recognizer_new_grm              recognizer_new_grm;
    ptr_vosk_recognizer_free                 recognizer_free;
    ptr_vosk_recognizer_set_max_alternatives set_max_alternatives;
    ptr_vosk_recognizer_accept_waveform_s    accept_waveform_s;
    ptr_vosk_recognizer_final_result         final_result;
    ptr_vosk_recognizer_reset                reset;
    ptr_vosk_set_log_level                   set_log_level;
};

struct vru_word
{
    char text[VRU_WORD_CHARS];
    int  slot;
};

/* A word as the recognizer was built to hear it. */
struct vru_grammar_word
{
    char text[VRU_WORD_CHARS];
    /* Japanese only: the lexicon words it was given to Vosk as, folded to
     * katakana with a space either side, or empty when it could not be. */
    char key[VRU_PHRASE_CHARS];
    int  slot;
};

/* The core options, kept apart from the rest: they arrive whenever the player
 * changes one, which can be before content is loaded, and loading content
 * resets everything else. */
static struct
{
    int   verbose;
    int   forced_language;     /* 0 auto, 1 english, 2 japanese */
    float gain;
} g_opt = {0, 0, 1.0f};

static struct
{
    /* set up once, on the frontend thread */
    char system_dir[1024];
    int  started;

    /* the library and the model, touched only by the worker */
    void*           lib;
    struct vru_vosk fn;
    VoskModel*      model;
    VoskRecognizer* recognizer;
    char            model_loaded[16];
    int             available;
    int             load_failed;

    /* The worker's own copy of the list: the game may start sending the next
     * one while this recognizer is still answering for the last. */
    struct vru_grammar_word grammar[VRU_MAX_WORDS];
    int                     grammar_count;
    int                     grammar_japanese;

    /* the microphone, touched only by the frontend thread */
    struct retro_microphone_interface mic_iface;
    retro_microphone_t*               mic;
    int                               mic_open;
    int                               mic_on;
    double                            mic_rate;
    int                               warned_late;
    int                               attached[4];
    int                               channels_built;

    /* shared, under lock */
    pthread_mutex_t lock;
    pthread_cond_t  wake;
    pthread_t       worker;
    int             worker_running;
    int             quit;

    struct vru_word words[VRU_MAX_WORDS];
    int             word_count;
    int             word_expected;
    unsigned        grammar_gen;
    unsigned        grammar_built;
    int             wants_japanese;

    int16_t*  ring;
    unsigned  ring_size;
    unsigned  ring_head;
    unsigned  ring_fill;

    int listening;
    int decode_pending;
    /* What the game last told the unit: listen, or stop. */
    int game_listening;
    /* From the game's stop until that utterance's result is published. */
    int decode_outstanding;
    int result_ready;
    int result_error;
    int result_count;
    uint16_t result_matches[VRU_MAX_MATCHES * 2];
} g_vru;

/* ------------------------------------------------------------------ */
/* the library                                                        */
/* ------------------------------------------------------------------ */

static void* vru_dlopen(const char* path)
{
#ifdef _WIN32
    return (void*)LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void* vru_dlsym(void* lib, const char* name)
{
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

static void vru_dlclose(void* lib)
{
    if (lib == NULL)
        return;
#ifdef _WIN32
    FreeLibrary((HMODULE)lib);
#else
    dlclose(lib);
#endif
}

static const char* vru_library_name(void)
{
#if defined(_WIN32)
    return "libvosk.dll";
#elif defined(__APPLE__)
    return "libvosk.dylib";
#else
    return "libvosk.so";
#endif
}

/* Everything the VRU needs sits under <system dir>/vru, which is per core
 * name, so the Windows and Android builds never collide.
 *
 * One separator throughout: LoadLibraryEx works out which directory to search
 * for a dependency from the path it is given, and libvosk brings its own. */
static void vru_path(char* out, size_t size, const char* leaf)
{
    size_t i;
    snprintf(out, size, "%s/vru/%s", g_vru.system_dir, leaf);
#ifdef _WIN32
    for (i = 0; out[i] != '\0'; i++)
    {
        if (out[i] == '/')
            out[i] = '\\';
    }
#else
    (void)i;
#endif
}

static int vru_load_library(void)
{
    static const char* symbols[] = {
        "vosk_model_new", "vosk_model_free", "vosk_recognizer_new_grm",
        "vosk_recognizer_free", "vosk_recognizer_set_max_alternatives",
        "vosk_recognizer_accept_waveform_s", "vosk_recognizer_final_result",
        "vosk_recognizer_reset", "vosk_set_log_level"
    };
    void** slots[] = {
        (void**)&g_vru.fn.model_new, (void**)&g_vru.fn.model_free,
        (void**)&g_vru.fn.recognizer_new_grm, (void**)&g_vru.fn.recognizer_free,
        (void**)&g_vru.fn.set_max_alternatives, (void**)&g_vru.fn.accept_waveform_s,
        (void**)&g_vru.fn.final_result, (void**)&g_vru.fn.reset,
        (void**)&g_vru.fn.set_log_level
    };
    char path[1024];
    size_t i;

    vru_path(path, sizeof(path), vru_library_name());

    g_vru.lib = vru_dlopen(path);
    if (g_vru.lib == NULL)
    {
#ifdef _WIN32
        DebugMessage(M64MSG_WARNING,
                     "VRU: could not load \"%s\" (error %lu); the unit will hear nothing",
                     path, (unsigned long)GetLastError());
#else
        DebugMessage(M64MSG_WARNING,
                     "VRU: could not load \"%s\" (%s); the unit will hear nothing",
                     path, dlerror());
#endif
        return 0;
    }

    for (i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++)
    {
        *slots[i] = vru_dlsym(g_vru.lib, symbols[i]);
        if (*slots[i] == NULL)
        {
            DebugMessage(M64MSG_WARNING, "VRU: \"%s\" is missing %s", path, symbols[i]);
            vru_dlclose(g_vru.lib);
            g_vru.lib = NULL;
            return 0;
        }
    }

    /* Optional: a plain recognizer is the fallback when a grammar will not
     * build, which happens when a word is not in the model's lexicon. */
    g_vru.fn.recognizer_new = (ptr_vosk_recognizer_new)vru_dlsym(g_vru.lib, "vosk_recognizer_new");

    g_vru.fn.set_log_level(-1);
    return 1;
}

static int vru_load_model(int japanese)
{
    const char* dir = japanese ? "model-ja" : "model-en-us";
    char path[1024];

    if (g_vru.model != NULL && strcmp(g_vru.model_loaded, dir) == 0)
        return 1;

    if (g_vru.recognizer != NULL)
    {
        g_vru.fn.recognizer_free(g_vru.recognizer);
        g_vru.recognizer = NULL;
    }
    if (g_vru.model != NULL)
    {
        g_vru.fn.model_free(g_vru.model);
        g_vru.model = NULL;
    }

    vru_path(path, sizeof(path), dir);
    g_vru.model = g_vru.fn.model_new(path);
    if (g_vru.model == NULL)
    {
        DebugMessage(M64MSG_WARNING, "VRU: no speech model at \"%s\"", path);
        return 0;
    }

    snprintf(g_vru.model_loaded, sizeof(g_vru.model_loaded), "%s", dir);
    DebugMessage(M64MSG_WARNING, "VRU: listening with %s", dir);
    return 1;
}

/* ------------------------------------------------------------------ */
/* the word table                                                     */
/* ------------------------------------------------------------------ */

static int vru_table_compare(const void* key, const void* row)
{
    return strcmp((const char*)key, ((const struct vru_word_entry*)row)->hex);
}

/* The game sends a word as big-endian uint16 phoneme codes; the table is keyed
 * by the hex of those bytes. */
static const char* vru_lookup(const uint16_t* word, uint16_t length)
{
    char hex[VRU_WORD_CHARS];
    const struct vru_word_entry* found;
    unsigned i;
    size_t at = 0;

    if (length == 0 || (size_t)length * 4 >= sizeof(hex))
        return NULL;

    for (i = 0; i < length; i++)
    {
        const uint8_t* bytes = (const uint8_t*)&word[i];
        at += snprintf(hex + at, sizeof(hex) - at, "%02X%02X", bytes[1], bytes[0]);
    }

    found = (const struct vru_word_entry*)bsearch(hex, g_vru_word_table,
                                                  VRU_WORD_TABLE_COUNT,
                                                  sizeof(g_vru_word_table[0]),
                                                  vru_table_compare);
    return (found != NULL) ? found->text : NULL;
}

/* ------------------------------------------------------------------ */
/* Japanese                                                           */
/* ------------------------------------------------------------------ */

/* The kana of Shift-JIS, which is all a word list is spelled in. The katakana
 * row steps over 0x837F because a trail byte is never 0x7F. */
static unsigned vru_sjis_to_unicode(unsigned code)
{
    if (code >= 0x829F && code <= 0x82F1)
        return 0x3041 + (code - 0x829F);
    if (code >= 0x8340 && code <= 0x837E)
        return 0x30A1 + (code - 0x8340);
    if (code >= 0x8380 && code <= 0x8396)
        return 0x30E0 + (code - 0x8380);
    if (code == 0x815B)
        return 0x30FC;
    if (code == 0x8140)
        return 0x3000;
    return 0;
}

static int vru_sjis_lead(unsigned byte)
{
    return (byte >= 0x81 && byte <= 0x9F) || (byte >= 0xE0 && byte <= 0xEF);
}

/* Only three-byte sequences are decoded, since every kana is one; anything
 * else comes back a byte at a time, to be copied through as it is. */
static unsigned vru_utf8_next(const char* text, size_t* length)
{
    const unsigned char* u = (const unsigned char*)text;

    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80)
    {
        *length = 3;
        return ((unsigned)(u[0] & 0x0F) << 12) | ((unsigned)(u[1] & 0x3F) << 6)
             | (unsigned)(u[2] & 0x3F);
    }
    *length = 1;
    return u[0];
}

/* ASCII or a three-byte character; returns 0 when it and a terminator do not
 * both fit. */
static size_t vru_utf8_put(char* out, size_t at, size_t size, unsigned cp)
{
    if (cp < 0x80)
    {
        if (at + 1 >= size)
            return 0;
        out[at] = (char)cp;
        return 1;
    }
    if (at + 3 >= size)
        return 0;
    out[at]     = (char)(0xE0 | (cp >> 12));
    out[at + 1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[at + 2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* The Japanese titles send a word as Shift-JIS, one character to each uint16,
 * where the English one sends phoneme codes. It is converted to UTF-8 here,
 * because that is what Vosk reads and answers in: copied byte for byte, the
 * grammar was in one encoding and the answers in another, and no Japanese word
 * could ever match. */
int vru_recognizer_decode_word(const uint16_t* word, uint16_t length,
                               char* out, size_t size, unsigned* refused)
{
    size_t at = 0;
    unsigned i;
    int japanese = 0;

    *refused = 0;
    for (i = 0; i < length; i++)
    {
        const uint8_t* bytes = (const uint8_t*)&word[i];
        unsigned cp;
        size_t put;

        if (bytes[0] == 0 || bytes[1] == 0)
        {
            cp = (unsigned)(bytes[0] | bytes[1]);
            if (cp == 0)
                continue;
            if (cp < 0x20 || cp >= 0x80)
                return 0;
        }
        else
        {
            unsigned code = ((unsigned)bytes[1] << 8) | bytes[0];
            cp = vru_sjis_to_unicode(code);
            if (cp == 0)
            {
                /* Not kana. Only worth naming when it was Shift-JIS at all:
                 * a phoneme code the table lacks lands here too. */
                if (vru_sjis_lead(bytes[1]))
                    *refused = code;
                return 0;
            }
            japanese = 1;
        }

        put = vru_utf8_put(out, at, size, cp);
        if (put == 0)
            return 0;
        at += put;
    }
    out[at] = '\0';
    return japanese;
}

/* Hiragana and katakana are one syllabary written twice. A game spells its
 * words in hiragana; the model's lexicon spells a word in whichever one it was
 * collected in, ピカチュウ only in katakana and こんにちわ in both. So words are
 * compared in katakana. Spaces separate the lexicon's words and mean nothing to
 * the game: they are dropped, or with keep_spaces written as one ASCII space. */
int vru_recognizer_fold_kana(const char* text, char* out, size_t size, int keep_spaces)
{
    size_t at = 0;

    while (*text != '\0')
    {
        size_t length;
        unsigned cp = vru_utf8_next(text, &length);
        size_t put;

        if (cp == ' ' || cp == 0x3000)
        {
            text += length;
            if (!keep_spaces)
                continue;
            cp = ' ';
        }
        else if ((cp >= 0x3041 && cp <= 0x3096) || cp == 0x309D || cp == 0x309E)
        {
            text += length;
            cp += 0x60;
        }
        else
        {
            if (at + length >= size)
                return 0;
            memcpy(out + at, text, length);
            at += length;
            text += length;
            continue;
        }

        put = vru_utf8_put(out, at, size, cp);
        if (put == 0)
            return 0;
        at += put;
    }
    out[at] = '\0';
    return 1;
}

/* A stretch of a game's folded word, and how the lexicon spells it if the
 * lexicon has it. */
struct vru_piece
{
    char* text;
    char* spelling;
    int   katakana;
};

static int vru_piece_order(const void* a, const void* b)
{
    return strcmp(((const struct vru_piece*)a)->text, ((const struct vru_piece*)b)->text);
}

static int vru_piece_compare(const void* key, const void* row)
{
    return strcmp((const char*)key, ((const struct vru_piece*)row)->text);
}

/* Where each character of a word starts, and where the last one ends. */
static int vru_char_offsets(const char* text, size_t* offsets)
{
    int count = 0;
    size_t at = 0;

    while (text[at] != '\0' && count < VRU_WORD_CHARS)
    {
        size_t length;
        offsets[count++] = at;
        vru_utf8_next(text + at, &length);
        at += length;
    }
    offsets[count] = at;
    return count;
}

/* A grammar is built out of the words in the model's lexicon, and a word the
 * lexicon lacks is left out of it. Most of what a Japanese title sends is not
 * one of those words -- the Pikachu game sends ぴかっちゅう and ぴかてう beside
 * ぴかちゅう, the ways a child might say it -- but nearly every one is a run of
 * several: ピカ ッチュウ. So each word goes to Vosk as the fewest lexicon words
 * that spell it. When the lexicon has both spellings of a stretch, the
 * katakana one is given: a hiragana entry can be a particle, and は read as a
 * particle is "wa".
 *
 * The lexicon is 200,000 lines and is not kept. The stretches of the game's
 * words are gathered first, and each line is only looked up among them.
 *
 * Returns how many words were spelled, or -1 when words.txt cannot be read.
 * A word that cannot be spelled gets an empty phrase. */
static int vru_segment_words(const char* words_path, const char* const* texts, int count,
                             char phrases[][VRU_PHRASE_CHARS])
{
    char* folded;
    struct vru_piece* pieces = NULL;
    char* blob = NULL;
    size_t piece_count = 0, bytes = 0, at = 0, kept, p;
    char line[512];
    FILE* file;
    int spelled = 0;
    int w;

    for (w = 0; w < count; w++)
        phrases[w][0] = '\0';

    file = fopen(words_path, "rb");
    if (file == NULL)
        return -1;

    folded = (char*)calloc((size_t)(count > 0 ? count : 1), VRU_WORD_CHARS);
    if (folded == NULL)
    {
        fclose(file);
        return 0;
    }

    for (w = 0; w < count; w++)
    {
        size_t offsets[VRU_WORD_CHARS + 1];
        int n, i, j;

        if (!vru_recognizer_fold_kana(texts[w], folded + w * VRU_WORD_CHARS, VRU_WORD_CHARS, 0))
            folded[w * VRU_WORD_CHARS] = '\0';
        n = vru_char_offsets(folded + w * VRU_WORD_CHARS, offsets);
        for (i = 0; i < n; i++)
        {
            for (j = i + 1; j <= n; j++)
            {
                piece_count++;
                bytes += 2 * (offsets[j] - offsets[i] + 1);
            }
        }
    }

    if (piece_count > 0)
    {
        pieces = (struct vru_piece*)malloc(piece_count * sizeof(pieces[0]));
        blob = (char*)malloc(bytes);
    }
    if (pieces == NULL || blob == NULL)
    {
        free(pieces);
        free(blob);
        free(folded);
        fclose(file);
        return 0;
    }

    piece_count = 0;
    for (w = 0; w < count; w++)
    {
        const char* word = folded + w * VRU_WORD_CHARS;
        size_t offsets[VRU_WORD_CHARS + 1];
        int n = vru_char_offsets(word, offsets), i, j;

        for (i = 0; i < n; i++)
        {
            for (j = i + 1; j <= n; j++)
            {
                size_t length = offsets[j] - offsets[i];
                struct vru_piece* piece = &pieces[piece_count++];
                piece->text = blob + at;
                memcpy(piece->text, word + offsets[i], length);
                piece->text[length] = '\0';
                at += length + 1;
                piece->spelling = blob + at;
                piece->spelling[0] = '\0';
                at += length + 1;
                piece->katakana = 0;
            }
        }
    }

    qsort(pieces, piece_count, sizeof(pieces[0]), vru_piece_order);
    for (p = 0, kept = 0; p < piece_count; p++)
    {
        if (kept == 0 || strcmp(pieces[kept - 1].text, pieces[p].text) != 0)
            pieces[kept++] = pieces[p];
    }
    piece_count = kept;

    /* Each line is "<word> <id>". */
    while (fgets(line, sizeof(line), file) != NULL)
    {
        char key[VRU_WORD_CHARS];
        char* end = strchr(line, ' ');
        struct vru_piece* piece;
        size_t length;
        int katakana;

        if (end == NULL)
            continue;
        *end = '\0';
        length = (size_t)(end - line);
        if (length >= sizeof(key) || !vru_recognizer_fold_kana(line, key, sizeof(key), 0)
            || strlen(key) != length)
            continue;

        piece = (struct vru_piece*)bsearch(key, pieces, piece_count, sizeof(pieces[0]),
                                           vru_piece_compare);
        if (piece == NULL)
            continue;

        katakana = (strcmp(line, key) == 0);
        if (piece->spelling[0] == '\0' || (katakana && !piece->katakana))
        {
            memcpy(piece->spelling, line, length + 1);
            piece->katakana = katakana;
        }
    }
    fclose(file);

    for (w = 0; w < count; w++)
    {
        const char* word = folded + w * VRU_WORD_CHARS;
        size_t offsets[VRU_WORD_CHARS + 1];
        int fewest[VRU_WORD_CHARS + 1];
        int from[VRU_WORD_CHARS + 1];
        const char* spelling[VRU_WORD_CHARS + 1];
        int chain[VRU_WORD_CHARS + 1];
        int n = vru_char_offsets(word, offsets), i, j, links = 0;
        size_t written = 0;
        int fits = 1;

        if (n == 0)
            continue;

        fewest[0] = 0;
        for (j = 1; j <= n; j++)
        {
            fewest[j] = -1;
            for (i = 0; i < j; i++)
            {
                char stretch[VRU_WORD_CHARS];
                size_t length = offsets[j] - offsets[i];
                const struct vru_piece* piece;

                if (fewest[i] < 0 || (fewest[j] >= 0 && fewest[i] + 1 >= fewest[j]))
                    continue;
                memcpy(stretch, word + offsets[i], length);
                stretch[length] = '\0';
                piece = (const struct vru_piece*)bsearch(stretch, pieces, piece_count,
                                                         sizeof(pieces[0]), vru_piece_compare);
                if (piece == NULL || piece->spelling[0] == '\0')
                    continue;
                fewest[j] = fewest[i] + 1;
                from[j] = i;
                spelling[j] = piece->spelling;
            }
        }
        if (fewest[n] < 0)
            continue;

        for (j = n; j > 0; j = from[j])
            chain[links++] = j;

        while (links-- > 0)
        {
            const char* piece = spelling[chain[links]];
            size_t length = strlen(piece);
            if (written + length + 2 > VRU_PHRASE_CHARS)
            {
                fits = 0;
                break;
            }
            if (written > 0)
                phrases[w][written++] = ' ';
            memcpy(phrases[w] + written, piece, length);
            written += length;
        }
        phrases[w][fits ? written : 0] = '\0';
        if (fits)
            spelled++;
    }

    free(pieces);
    free(blob);
    free(folded);
    return spelled;
}

int vru_recognizer_segment(const char* words_path, const char* text, char* phrase, size_t size)
{
    char phrases[1][VRU_PHRASE_CHARS];
    const char* texts[1];
    int spelled;

    texts[0] = text;
    spelled = vru_segment_words(words_path, texts, 1, phrases);
    snprintf(phrase, size, "%s", phrases[0]);
    return spelled;
}

/* ------------------------------------------------------------------ */
/* the grammar                                                        */
/* ------------------------------------------------------------------ */

static char* vru_build_grammar(char phrases[][VRU_PHRASE_CHARS], int count)
{
    size_t size = 32;
    char* json;
    size_t at = 0;
    int i;

    for (i = 0; i < count; i++)
        size += strlen(phrases[i]) * 2 + 4;

    json = (char*)malloc(size);
    if (json == NULL)
        return NULL;

    json[at++] = '[';
    for (i = 0; i < count; i++)
    {
        const char* text = phrases[i];
        if (*text == '\0')
            continue;
        json[at++] = '"';
        while (*text != '\0')
        {
            if (*text == '"' || *text == '\\')
                json[at++] = '\\';
            json[at++] = *text++;
        }
        json[at++] = '"';
        json[at++] = ',';
    }
    /* vosk wants somewhere to put what it heard that was not asked for. */
    at += snprintf(json + at, size - at, "\"[unk]\"]");

    return json;
}

/* For a Japanese model, each word becomes the lexicon words that spell it, and
 * the match key those words fold to. A word none of them spell is given to
 * Vosk as nothing, and can never match. */
static void vru_spell_japanese(char phrases[][VRU_PHRASE_CHARS], int count)
{
    const char* texts[VRU_MAX_WORDS];
    char path[1024];
    int spelled, i;

    for (i = 0; i < count; i++)
        texts[i] = g_vru.grammar[i].text;

    vru_path(path, sizeof(path), "model-ja/graph/words.txt");
    spelled = vru_segment_words(path, texts, count, phrases);
    if (spelled < 0)
    {
        DebugMessage(M64MSG_WARNING,
                     "VRU: no word list at \"%s\"; a word model-ja does not know whole will not be heard",
                     path);
        for (i = 0; i < count; i++)
            snprintf(phrases[i], VRU_PHRASE_CHARS, "%s", texts[i]);
    }

    for (i = 0; i < count; i++)
    {
        struct vru_grammar_word* word = &g_vru.grammar[i];

        if (word->text[0] == '\0')
            continue;

        if (phrases[i][0] == '\0'
            || !vru_recognizer_fold_kana(phrases[i], word->key + 1, sizeof(word->key) - 2, 1))
        {
            word->key[0] = '\0';
            if (g_opt.verbose)
                DebugMessage(M64MSG_WARNING,
                             "VRU: word %d \"%s\" is not spelled by model-ja's words; it cannot be heard",
                             word->slot, word->text);
            continue;
        }
        word->key[0] = ' ';
        strcat(word->key, " ");

        /* One line a word: a grammar logged whole is cut off at the core's
         * message buffer long before a Japanese list ends. */
        if (g_opt.verbose)
            DebugMessage(M64MSG_WARNING, "VRU: word %d \"%s\" as \"%s\"",
                         word->slot, word->text, phrases[i]);
    }
}

static void vru_rebuild_recognizer(void)
{
    char (*phrases)[VRU_PHRASE_CHARS];
    char* grammar;
    int japanese = 0;
    int count, i;
    float rate = (float)VRU_CAPTURE_RATE;

    pthread_mutex_lock(&g_vru.lock);
    count = (g_vru.word_count < VRU_MAX_WORDS) ? g_vru.word_count : VRU_MAX_WORDS;
    for (i = 0; i < count; i++)
    {
        memcpy(g_vru.grammar[i].text, g_vru.words[i].text, sizeof(g_vru.grammar[i].text));
        g_vru.grammar[i].slot = g_vru.words[i].slot;
        g_vru.grammar[i].key[0] = '\0';
    }
    pthread_mutex_unlock(&g_vru.lock);
    g_vru.grammar_count = count;

    for (i = 0; i < count && !japanese; i++)
    {
        const char* text;
        for (text = g_vru.grammar[i].text; *text != '\0'; text++)
        {
            if ((unsigned char)*text >= 0x80)
                japanese = 1;
        }
    }

    if (g_opt.forced_language == 1)
        japanese = 0;
    else if (g_opt.forced_language == 2)
        japanese = 1;

    if (!vru_load_model(japanese))
        return;
    g_vru.grammar_japanese = japanese;

    /* The recognizer goes whatever happens next: it was built for a list the
     * copy above has already replaced. */
    if (g_vru.recognizer != NULL)
    {
        g_vru.fn.recognizer_free(g_vru.recognizer);
        g_vru.recognizer = NULL;
    }

    phrases = (char (*)[VRU_PHRASE_CHARS])calloc((size_t)(count > 0 ? count : 1), VRU_PHRASE_CHARS);
    if (phrases == NULL)
        return;

    if (japanese)
        vru_spell_japanese(phrases, count);
    else
    {
        for (i = 0; i < count; i++)
            snprintf(phrases[i], VRU_PHRASE_CHARS, "%s", g_vru.grammar[i].text);
    }

    grammar = vru_build_grammar(phrases, count);
    free(phrases);
    if (grammar == NULL)
        return;

    /* Vosk resamples to its model's rate itself, so it is handed the capture
     * rate rather than a decimated stream. */
    g_vru.recognizer = g_vru.fn.recognizer_new_grm(g_vru.model, rate, grammar);
    if (g_vru.recognizer == NULL && g_vru.fn.recognizer_new != NULL)
    {
        /* Vosk leaves a word it has no entry for out of the grammar rather than
         * failing, so this is a grammar it could not read at all. */
        DebugMessage(M64MSG_WARNING,
                     "VRU: the game's word list would not build a grammar; listening without one");
        g_vru.recognizer = g_vru.fn.recognizer_new(g_vru.model, rate);
    }

    if (g_vru.recognizer != NULL)
        g_vru.fn.set_max_alternatives(g_vru.recognizer, 3);
    else
        DebugMessage(M64MSG_WARNING, "VRU: vosk_recognizer_new_grm failed");

    if (g_opt.verbose)
        DebugMessage(M64MSG_WARNING, "VRU: grammar %s", grammar);

    free(grammar);
}

/* ------------------------------------------------------------------ */
/* results                                                            */
/* ------------------------------------------------------------------ */

/* Vosk answers {"alternatives":[{"confidence":..,"text":".."},..]}. Only the
 * texts matter, and they arrive best first. */
static int vru_collect_alternatives(const char* json, char out[3][256])
{
    int found = 0;
    const char* at = json;

    while (found < 3 && (at = strstr(at, "\"text\"")) != NULL)
    {
        size_t n = 0;
        at = strchr(at + 6, '"');
        if (at == NULL)
            break;
        at++;
        while (*at != '\0' && *at != '"' && n + 1 < sizeof(out[0]))
        {
            if (*at == '\\' && at[1] != '\0')
                at++;
            out[found][n++] = *at++;
        }
        out[found][n] = '\0';
        if (n > 0)
            found++;
    }

    return found;
}

struct vru_match
{
    const struct vru_grammar_word* word;
    int                            rank;    /* the best alternative it was heard in */
};

/* The longest match wins: "pikachu" beats "pika" when both are in the list and
 * the player said the longer one. Between two of a length -- a Japanese list is
 * full of spellings like ぴかっちゅ and ぴかっちう -- the one from Vosk's likelier
 * alternative, and then the lower slot, so the order is never left to qsort. */
static int vru_match_order(const void* a, const void* b)
{
    const struct vru_match* ma = (const struct vru_match*)a;
    const struct vru_match* mb = (const struct vru_match*)b;
    size_t la = strlen(ma->word->text), lb = strlen(mb->word->text);
    if (la != lb)
        return (la < lb) ? 1 : -1;
    if (ma->rank != mb->rank)
        return (ma->rank > mb->rank) ? 1 : -1;
    if (ma->word->slot != mb->word->slot)
        return (ma->word->slot > mb->word->slot) ? 1 : -1;
    return 0;
}

/* Called on the worker with the lock held only around the publish. */
static void vru_publish(const char* json)
{
    char alternatives[3][256];
    char keys[3][VRU_PHRASE_CHARS];
    struct vru_match found[VRU_MAX_WORDS];
    int alternative_count;
    int found_count = 0;
    int i, j, k;
    uint16_t matches[VRU_MAX_MATCHES * 2];
    int error = 0;

    if (g_opt.verbose)
        DebugMessage(M64MSG_WARNING, "VRU: vosk said %s", json);

    alternative_count = vru_collect_alternatives(json, alternatives);

    /* Vosk answers in lexicon words, and one sound can be several of them --
     * "ピカ チュウ" or "ぴか ちゅう". Folded and matched a whole word at a time,
     * ぴかってう is not also heard as the かって inside it. */
    for (j = 0; j < alternative_count && g_vru.grammar_japanese; j++)
    {
        keys[j][0] = ' ';
        if (!vru_recognizer_fold_kana(alternatives[j], keys[j] + 1, sizeof(keys[j]) - 2, 1))
            keys[j][1] = '\0';
        strcat(keys[j], " ");
    }

    for (i = 0; i < g_vru.grammar_count; i++)
    {
        const struct vru_grammar_word* word = &g_vru.grammar[i];
        int already = 0;

        /* A word that could not be read holds its slot and nothing else. */
        if (word->text[0] == '\0')
            continue;

        for (j = 0; j < found_count; j++)
        {
            if (strcmp(found[j].word->text, word->text) == 0)
                already = 1;
        }
        if (already)
            continue;

        for (j = 0; j < alternative_count; j++)
        {
            int heard = g_vru.grammar_japanese
                ? (word->key[0] != '\0' && strstr(keys[j], word->key) != NULL)
                : (strstr(alternatives[j], word->text) != NULL);
            if (heard)
            {
                found[found_count].word = word;
                found[found_count].rank = j;
                found_count++;
                break;
            }
        }
    }

    /* Ordered before the reply is cut to five, or the five would be the lowest
     * slots rather than the best matches. */
    if (found_count > 1)
        qsort(found, (size_t)found_count, sizeof(found[0]), vru_match_order);
    if (found_count > VRU_MAX_MATCHES)
        found_count = VRU_MAX_MATCHES;

    for (i = 0, k = 0; i < VRU_MAX_MATCHES; i++)
    {
        if (i < found_count)
        {
            matches[k++] = (uint16_t)found[i].word->slot;
            matches[k++] = (uint16_t)(i * 256);
        }
        else
        {
            matches[k++] = VRU_MATCH_EMPTY;
            matches[k++] = 0;
        }
    }

    if (found_count == 0 && alternative_count > 0)
    {
        /* Heard, but nothing the game is listening for. */
        error = VRU_ERROR_NO_MATCH;
        matches[0] = 0;
        found_count = 1;
    }

    if (g_opt.verbose && error == VRU_ERROR_NO_MATCH)
        DebugMessage(M64MSG_WARNING, "VRU: heard \"%s\", which the game is not listening for",
                     alternatives[0]);
    else if (g_opt.verbose && found_count > 0)
        DebugMessage(M64MSG_WARNING, "VRU: heard \"%s\" as word %d \"%s\"",
                     alternatives[0], found[0].word->slot, found[0].word->text);

    pthread_mutex_lock(&g_vru.lock);
    memcpy(g_vru.result_matches, matches, sizeof(matches));
    g_vru.result_count = found_count;
    g_vru.result_error = error;
    g_vru.result_ready = 1;
    g_vru.decode_outstanding = 0;
    pthread_mutex_unlock(&g_vru.lock);
}

/* ------------------------------------------------------------------ */
/* the worker                                                         */
/* ------------------------------------------------------------------ */

static void* vru_worker(void* unused)
{
    int16_t block[4096];

    (void)unused;

    for (;;)
    {
        unsigned take = 0;
        int decode = 0;
        unsigned gen;

        pthread_mutex_lock(&g_vru.lock);
        while (!g_vru.quit && g_vru.ring_fill == 0 && !g_vru.decode_pending
               && g_vru.grammar_gen == g_vru.grammar_built)
            pthread_cond_wait(&g_vru.wake, &g_vru.lock);

        if (g_vru.quit)
        {
            pthread_mutex_unlock(&g_vru.lock);
            break;
        }

        gen = g_vru.grammar_gen;
        if (g_vru.ring_fill > 0)
        {
            take = g_vru.ring_fill;
            if (take > sizeof(block) / sizeof(block[0]))
                take = sizeof(block) / sizeof(block[0]);
            memcpy(block, g_vru.ring + g_vru.ring_head, take * sizeof(int16_t));
            g_vru.ring_head += take;
            g_vru.ring_fill -= take;
            if (g_vru.ring_fill == 0)
                g_vru.ring_head = 0;
        }
        /* Only once the ring is empty: the utterance ends the moment the player
         * lets go, but the last of it is still queued, and deciding on a
         * fraction of a word is deciding on nothing. */
        decode = g_vru.decode_pending && g_vru.ring_fill == 0;
        if (decode)
            g_vru.decode_pending = 0;
        pthread_mutex_unlock(&g_vru.lock);

        if (gen != g_vru.grammar_built)
        {
            vru_rebuild_recognizer();
            pthread_mutex_lock(&g_vru.lock);
            g_vru.grammar_built = gen;
            pthread_mutex_unlock(&g_vru.lock);
        }

        if (g_vru.recognizer == NULL)
        {
            /* No model or no list: there will be no result to wait for. */
            if (decode)
            {
                pthread_mutex_lock(&g_vru.lock);
                g_vru.decode_outstanding = 0;
                pthread_mutex_unlock(&g_vru.lock);
            }
            continue;
        }

        if (take > 0)
            g_vru.fn.accept_waveform_s(g_vru.recognizer, block, (int)take);

        if (decode)
        {
            const char* json = g_vru.fn.final_result(g_vru.recognizer);
            if (json != NULL)
                vru_publish(json);
            else
            {
                pthread_mutex_lock(&g_vru.lock);
                g_vru.decode_outstanding = 0;
                pthread_mutex_unlock(&g_vru.lock);
            }
            g_vru.fn.reset(g_vru.recognizer);
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* lifecycle, on the frontend thread                                  */
/* ------------------------------------------------------------------ */

void vru_recognizer_init(const char* system_dir)
{
    if (g_vru.started)
        return;

    memset(&g_vru, 0, sizeof(g_vru));
    snprintf(g_vru.system_dir, sizeof(g_vru.system_dir), "%s",
             (system_dir != NULL) ? system_dir : ".");
    pthread_mutex_init(&g_vru.lock, NULL);
    pthread_cond_init(&g_vru.wake, NULL);
    g_vru.started = 1;
}

static void vru_stop_worker(void)
{
    if (!g_vru.worker_running)
        return;

    pthread_mutex_lock(&g_vru.lock);
    g_vru.quit = 1;
    pthread_cond_signal(&g_vru.wake);
    pthread_mutex_unlock(&g_vru.lock);
    pthread_join(g_vru.worker, NULL);
    g_vru.worker_running = 0;
}

/* Give the model back. Without a microphone there is nothing to decode, and a
 * loaded model is most of a third of a gigabyte. */
static void vru_stop_engine(void)
{
    vru_stop_worker();

    if (g_vru.lib != NULL)
    {
        if (g_vru.recognizer != NULL)
            g_vru.fn.recognizer_free(g_vru.recognizer);
        if (g_vru.model != NULL)
            g_vru.fn.model_free(g_vru.model);
        vru_dlclose(g_vru.lib);
    }
    g_vru.recognizer = NULL;
    g_vru.model = NULL;
    g_vru.lib = NULL;
    g_vru.model_loaded[0] = '\0';

    free(g_vru.ring);
    g_vru.ring = NULL;
    g_vru.ring_head = 0;
    g_vru.ring_fill = 0;
}

void vru_recognizer_close(void)
{
    if (!g_vru.started)
        return;

    vru_stop_worker();

    if (g_vru.mic_open && g_vru.mic_iface.close_mic != NULL)
        g_vru.mic_iface.close_mic(g_vru.mic);
    g_vru.mic = NULL;
    g_vru.mic_open = 0;

    if (g_vru.lib != NULL)
    {
        if (g_vru.recognizer != NULL)
            g_vru.fn.recognizer_free(g_vru.recognizer);
        if (g_vru.model != NULL)
            g_vru.fn.model_free(g_vru.model);
        vru_dlclose(g_vru.lib);
    }
    g_vru.recognizer = NULL;
    g_vru.model = NULL;
    g_vru.lib = NULL;

    free(g_vru.ring);
    g_vru.ring = NULL;

    pthread_mutex_destroy(&g_vru.lock);
    pthread_cond_destroy(&g_vru.wake);
    g_vru.started = 0;
}

void vru_recognizer_set_options(const char* language, float gain, int verbose)
{
    g_opt.forced_language = 0;
    if (language != NULL && strcmp(language, "english") == 0)
        g_opt.forced_language = 1;
    else if (language != NULL && strcmp(language, "japanese") == 0)
        g_opt.forced_language = 2;

    g_opt.gain = (gain > 0.0f) ? gain : 1.0f;
    g_opt.verbose = verbose;
}

static int vru_any_port(void)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        if (pad_types[i] == CONT_TYPE_VRU)
            return i;
    }
    return -1;
}

static void vru_stop_worker(void);
static void vru_stop_engine(void);

/* The library, the ring and the worker: everything except the microphone. */
static int vru_start_engine(void)
{
    if (g_vru.load_failed)
        return 0;

    if (!vru_load_library())
    {
        g_vru.load_failed = 1;
        return 0;
    }

    g_vru.ring_size = VRU_CAPTURE_RATE * VRU_RING_SECONDS;
    g_vru.ring = (int16_t*)calloc(g_vru.ring_size, sizeof(int16_t));
    if (g_vru.ring == NULL)
    {
        g_vru.load_failed = 1;
        return 0;
    }

    if (pthread_create(&g_vru.worker, NULL, vru_worker, NULL) != 0)
    {
        DebugMessage(M64MSG_WARNING, "VRU: could not start the recognizer thread");
        g_vru.load_failed = 1;
        return 0;
    }
    g_vru.worker_running = 1;
    return 1;
}

int vru_recognizer_start_offline(const char* system_dir)
{
    vru_recognizer_init(system_dir);
    if (!vru_start_engine())
        return 0;
    g_vru.mic_rate = VRU_CAPTURE_RATE;
    g_vru.available = 1;
    return 1;
}

static int vru_start(void)
{
    struct retro_microphone_params params;

    if (!vru_start_engine())
        return 0;

    g_vru.mic_iface.interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE, &g_vru.mic_iface)
        || g_vru.mic_iface.open_mic == NULL)
    {
        DebugMessage(M64MSG_WARNING, "VRU: this frontend offers no microphone");
        g_vru.load_failed = 1;
        return 0;
    }

    params.rate = VRU_CAPTURE_RATE;
    g_vru.mic = g_vru.mic_iface.open_mic(&params);
    if (g_vru.mic == NULL)
    {
        DebugMessage(M64MSG_WARNING, "VRU: the frontend would not open a microphone");
        g_vru.load_failed = 1;
        vru_stop_engine();
        return 0;
    }
    if (g_vru.mic_iface.get_params != NULL && g_vru.mic_iface.get_params(g_vru.mic, &params))
        g_vru.mic_rate = params.rate;
    else
        g_vru.mic_rate = VRU_CAPTURE_RATE;
    g_vru.mic_open = 1;
    g_vru.available = 1;
    return 1;
}

void vru_recognizer_push_pcm(const int16_t* samples, unsigned count)
{
    unsigned room;

    if (g_vru.ring == NULL || count == 0)
        return;

    pthread_mutex_lock(&g_vru.lock);
    room = g_vru.ring_size - (g_vru.ring_head + g_vru.ring_fill);
    if (room < count && g_vru.ring_head > 0)
    {
        memmove(g_vru.ring, g_vru.ring + g_vru.ring_head,
                g_vru.ring_fill * sizeof(int16_t));
        g_vru.ring_head = 0;
        room = g_vru.ring_size - g_vru.ring_fill;
    }
    if (count > room)
        count = room;
    memcpy(g_vru.ring + g_vru.ring_head + g_vru.ring_fill, samples,
           count * sizeof(int16_t));
    g_vru.ring_fill += count;
    pthread_cond_signal(&g_vru.wake);
    pthread_mutex_unlock(&g_vru.lock);
}

int vru_recognizer_is_available(void)
{
    return g_vru.available;
}

int vru_recognizer_grammar_ready(void)
{
    int ready;

    pthread_mutex_lock(&g_vru.lock);
    ready = (g_vru.grammar_gen == g_vru.grammar_built);
    pthread_mutex_unlock(&g_vru.lock);
    return ready;
}

void vru_recognizer_note_attached(int port, int is_vru)
{
    if (port < 0 || port > 3)
        return;

    g_vru.attached[port] = is_vru;
    g_vru.channels_built = 1;
    if (is_vru)
        DebugMessage(M64MSG_WARNING, "VRU: port %d is a Voice Recognition Unit", port + 1);
}

void vru_recognizer_pump(void)
{
    int16_t block[2048];
    int port;
    int listening;
    int want_on;
    int frames;

    if (!g_vru.started)
        return;

    port = vru_any_port();
    if (port < 0)
        return;

    if (!g_vru.available && !g_vru.load_failed && !vru_start())
        return;
    if (!g_vru.available)
        return;

    /* The joybus channels were built before this port became a VRU, so the
     * game is looking at a plain controller and will go on doing so. */
    if (!g_vru.warned_late && g_vru.channels_built && !g_vru.attached[port])
    {
        g_vru.warned_late = 1;
        DebugMessage(M64MSG_WARNING,
                     "VRU: port %d became a Voice Recognition Unit after the machine started; "
                     "reload the game for it to attach (a reset does not rebuild the channels)",
                     port + 1);
    }

    pthread_mutex_lock(&g_vru.lock);
    listening = g_vru.listening;
    pthread_mutex_unlock(&g_vru.lock);

    want_on = listening;
    if (want_on != g_vru.mic_on && g_vru.mic_iface.set_mic_state != NULL)
    {
        if (g_vru.mic_iface.set_mic_state(g_vru.mic, want_on != 0))
            g_vru.mic_on = want_on;
    }

    if (!g_vru.mic_on || g_vru.mic_iface.read_mic == NULL)
        return;

    frames = (int)(g_vru.mic_rate / 60.0);
    if (frames > (int)(sizeof(block) / sizeof(block[0])))
        frames = (int)(sizeof(block) / sizeof(block[0]));

    frames = g_vru.mic_iface.read_mic(g_vru.mic, block, (size_t)frames);
    if (frames <= 0)
        return;

    if (g_opt.gain != 1.0f)
    {
        int i;
        for (i = 0; i < frames; i++)
        {
            int scaled = (int)(block[i] * g_opt.gain);
            block[i] = (int16_t)((scaled > 32767) ? 32767 : (scaled < -32768 ? -32768 : scaled));
        }
    }

    vru_recognizer_push_pcm(block, (unsigned)frames);
}

void vru_recognizer_reset_words(void)
{
    if (!g_vru.started)
        return;

    pthread_mutex_lock(&g_vru.lock);
    g_vru.word_count = 0;
    g_vru.word_expected = 0;
    g_vru.result_ready = 0;
    g_vru.result_count = 0;
    g_vru.ring_head = 0;
    g_vru.ring_fill = 0;
    g_vru.listening = 0;
    g_vru.game_listening = 0;
    g_vru.decode_pending = 0;
    g_vru.decode_outstanding = 0;
    pthread_mutex_unlock(&g_vru.lock);
}

/* ------------------------------------------------------------------ */
/* the emulation thread                                               */
/* ------------------------------------------------------------------ */

int vru_recognizer_talking(int port, int held)
{
    int talking;

    (void)port;
    (void)held;

    if (!g_vru.started)
        return 0;

    pthread_mutex_lock(&g_vru.lock);
    talking = g_vru.game_listening;
    pthread_mutex_unlock(&g_vru.lock);
    return talking;
}

EXPORT void CALL vru_ClearVRUWords(uint8_t length)
{
    if (!g_vru.started)
        return;

    pthread_mutex_lock(&g_vru.lock);
    g_vru.word_count = 0;
    g_vru.word_expected = length;
    pthread_mutex_unlock(&g_vru.lock);
}

EXPORT void CALL vru_SendVRUWord(uint16_t length, uint16_t* word, uint8_t lang)
{
    const char* text;
    char decoded[VRU_WORD_CHARS];
    unsigned refused = 0;
    int slot;

    if (!g_vru.started || word == NULL)
        return;

    text = vru_lookup(word, length);
    if (text == NULL && vru_recognizer_decode_word(word, length, decoded, sizeof(decoded), &refused))
        text = decoded;

    if (text == NULL && g_opt.verbose)
    {
        if (refused != 0)
            DebugMessage(M64MSG_WARNING,
                         "VRU: word %d holds Shift-JIS %04X, which is not kana; it cannot be heard",
                         g_vru.word_count, refused);
        else
            DebugMessage(M64MSG_WARNING, "VRU: word %d is not in the table", g_vru.word_count);
    }

    /* A word that cannot be read still takes its slot, because the game counts
     * them, and is held empty: left alone, the slot would go on hearing the
     * word the last list had there. It also still completes the list. */
    pthread_mutex_lock(&g_vru.lock);
    slot = g_vru.word_count;
    if (slot < VRU_MAX_WORDS)
    {
        snprintf(g_vru.words[slot].text, sizeof(g_vru.words[slot].text), "%s",
                 (text != NULL) ? text : "");
        g_vru.words[slot].slot = slot;
    }
    g_vru.word_count = slot + 1;

    /* The list is only usable once the game has finished sending it. */
    if (g_vru.word_count == g_vru.word_expected)
    {
        g_vru.grammar_gen++;
        pthread_cond_signal(&g_vru.wake);
    }
    pthread_mutex_unlock(&g_vru.lock);

    (void)lang;
}

/* The game's own "listen" and "stop", which Hey You, Pikachu! sends while the
 * player holds the controller's Z. The utterance is exactly what the microphone
 * heard in between: the start throws away anything captured before it, and the
 * stop keeps what is still queued, because the game asks for the result as soon
 * as it has stopped listening. */
EXPORT void CALL vru_SetMicState(int state)
{
    int changed;

    if (!g_vru.started)
        return;

    state = (state != 0);
    pthread_mutex_lock(&g_vru.lock);
    changed = (state != g_vru.game_listening);
    g_vru.game_listening = state;
    if (changed && state)
    {
        g_vru.listening = 1;
        g_vru.result_ready = 0;
        g_vru.ring_head = 0;
        g_vru.ring_fill = 0;
    }
    else if (changed)
    {
        g_vru.listening = 0;
        if (g_vru.available)
        {
            g_vru.decode_pending = 1;
            g_vru.decode_outstanding = 1;
        }
        pthread_cond_signal(&g_vru.wake);
    }
    pthread_mutex_unlock(&g_vru.lock);

    if (changed && g_opt.verbose)
        DebugMessage(M64MSG_WARNING, "VRU: the game %s listening", state ? "started" : "stopped");
}

EXPORT void CALL vru_ReadVRUResults(uint16_t* error_flags, uint16_t* num_results,
                                    uint16_t* mic_level, uint16_t* voice_level,
                                    uint16_t* voice_length, uint16_t* matches)
{
    int waited = 0;
    int ready = 0;
    int i;

    /* Every field is written on every read. These point into the joybus reply,
     * which still holds whatever was there before, and a field left alone is a
     * field the game reads as stale bytes: error flags left over as 0xFE78 carry
     * both the decode-failed and the no-match bit. */
    if (error_flags != NULL)
        *error_flags = 0;
    if (num_results != NULL)
        *num_results = 0;
    if (mic_level != NULL)
        *mic_level = VRU_LEVEL_NOMINAL;
    if (voice_level != NULL)
        *voice_level = VRU_LEVEL_NOMINAL;
    if (voice_length != NULL)
        *voice_length = VRU_VOICE_LENGTH;
    if (matches != NULL)
    {
        for (i = 0; i < VRU_MAX_MATCHES * 2; i += 2)
        {
            matches[i]     = VRU_MATCH_EMPTY;
            matches[i + 1] = 0;
        }
    }

    if (!g_vru.started || !g_vru.available)
        return;

    /* The game asks the moment it stops listening, while the worker is still
     * decoding what was said; Rosalie's Mupen GUI and simple64 decode right
     * here, so the game is held for that long either way. */
    for (;;)
    {
        int outstanding;
        pthread_mutex_lock(&g_vru.lock);
        ready = g_vru.result_ready;
        outstanding = g_vru.decode_outstanding;
        pthread_mutex_unlock(&g_vru.lock);
        if (ready || !outstanding || waited >= VRU_RESULT_WAIT_MS)
            break;
        retro_sleep(10);
        waited += 10;
    }

    if (!ready)
    {
        if (g_opt.verbose)
            DebugMessage(M64MSG_WARNING, "VRU: the game asked for a result and there was none (%d ms)",
                         waited);
        return;
    }

    pthread_mutex_lock(&g_vru.lock);
    if (matches != NULL)
        memcpy(matches, g_vru.result_matches, sizeof(g_vru.result_matches));
    if (num_results != NULL)
        *num_results = (uint16_t)g_vru.result_count;
    if (error_flags != NULL && g_vru.result_error != 0)
        *error_flags = (uint16_t)g_vru.result_error;
    g_vru.result_ready = 0;
    pthread_mutex_unlock(&g_vru.lock);

    if (g_opt.verbose)
        DebugMessage(M64MSG_WARNING, "VRU: the game read %u result(s), first slot %u, flags %04X (%d ms)",
                     (num_results != NULL) ? *num_results : 0,
                     (matches != NULL) ? matches[0] : 0,
                     (error_flags != NULL) ? *error_flags : 0, waited);
}

EXPORT void CALL vru_SetVRUWordMask(uint8_t length, uint8_t* mask)
{
    /* The core has never called this. */
    (void)length;
    (void)mask;
}
