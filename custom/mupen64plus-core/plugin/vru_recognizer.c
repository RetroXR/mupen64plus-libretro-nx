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

#define VRU_MAX_WORDS       64
#define VRU_WORD_CHARS      96
#define VRU_CAPTURE_RATE    48000
#define VRU_RING_SECONDS    8
#define VRU_RESULT_WAIT_MS  250

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

/* The core options, kept apart from the rest: they arrive whenever the player
 * changes one, which can be before content is loaded, and loading content
 * resets everything else. */
static struct
{
    int   verbose;
    int   forced_language;     /* 0 auto, 1 english, 2 japanese */
    int   voice_activated;
    float gain;
} g_opt = {0, 0, 0, 1.0f};

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
#ifdef _WIN32
    return "libvosk.dll";
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

/* A word the table does not carry may still be readable: the Japanese titles
 * send text rather than phoneme codes. Accept it when it decodes, so the
 * grammar can carry it and the model can be picked from it. */
static int vru_decode_text(const uint16_t* word, uint16_t length, char* out, size_t size)
{
    size_t at = 0;
    unsigned i;
    int nonascii = 0;

    for (i = 0; i < length && at + 2 < size; i++)
    {
        const uint8_t* bytes = (const uint8_t*)&word[i];
        int b;
        for (b = 1; b >= 0; b--)
        {
            uint8_t c = bytes[b];
            if (c == 0)
                continue;
            if (c < 0x20)
                return 0;
            if (c >= 0x80)
                nonascii = 1;
            out[at++] = (char)c;
        }
    }
    out[at] = '\0';
    return (at > 0 && nonascii) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* the grammar                                                        */
/* ------------------------------------------------------------------ */

static char* vru_build_grammar(int* japanese)
{
    size_t size = 32;
    char* json;
    size_t at = 0;
    int i;
    int nonascii = 0;

    for (i = 0; i < g_vru.word_count; i++)
        size += strlen(g_vru.words[i].text) * 2 + 4;

    json = (char*)malloc(size);
    if (json == NULL)
        return NULL;

    json[at++] = '[';
    for (i = 0; i < g_vru.word_count; i++)
    {
        const char* text = g_vru.words[i].text;
        json[at++] = '"';
        while (*text != '\0')
        {
            if (*text == '"' || *text == '\\')
                json[at++] = '\\';
            if ((unsigned char)*text >= 0x80)
                nonascii = 1;
            json[at++] = *text++;
        }
        json[at++] = '"';
        json[at++] = ',';
    }
    /* vosk wants somewhere to put what it heard that was not asked for. */
    at += snprintf(json + at, size - at, "\"[unk]\"]");

    *japanese = nonascii;
    return json;
}

static void vru_rebuild_recognizer(void)
{
    char* grammar;
    int japanese = 0;
    float rate = (float)VRU_CAPTURE_RATE;

    grammar = vru_build_grammar(&japanese);
    if (grammar == NULL)
        return;

    if (g_opt.forced_language == 1)
        japanese = 0;
    else if (g_opt.forced_language == 2)
        japanese = 1;

    if (!vru_load_model(japanese))
    {
        free(grammar);
        return;
    }

    if (g_vru.recognizer != NULL)
    {
        g_vru.fn.recognizer_free(g_vru.recognizer);
        g_vru.recognizer = NULL;
    }

    /* Vosk resamples to its model's rate itself, so it is handed the capture
     * rate rather than a decimated stream. */
    g_vru.recognizer = g_vru.fn.recognizer_new_grm(g_vru.model, rate, grammar);
    if (g_vru.recognizer == NULL && g_vru.fn.recognizer_new != NULL)
    {
        /* A word outside the model's lexicon takes the whole grammar with it;
         * an open recognizer still hears the ones that are in it. */
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

static int vru_longer_first(const void* a, const void* b)
{
    const struct vru_word* wa = (const struct vru_word*)a;
    const struct vru_word* wb = (const struct vru_word*)b;
    size_t la = strlen(wa->text), lb = strlen(wb->text);
    if (la != lb)
        return (la < lb) ? 1 : -1;
    return 0;
}

/* Called on the worker with the lock held only around the publish. */
static void vru_publish(const char* json)
{
    char alternatives[3][256];
    struct vru_word found[VRU_MAX_MATCHES];
    int alternative_count;
    int found_count = 0;
    int i, j, k;
    uint16_t matches[VRU_MAX_MATCHES * 2];
    int error = 0;

    if (g_opt.verbose)
        DebugMessage(M64MSG_WARNING, "VRU: vosk said %s", json);

    alternative_count = vru_collect_alternatives(json, alternatives);

    for (i = 0; i < g_vru.word_count && found_count < VRU_MAX_MATCHES; i++)
    {
        int already = 0;
        for (j = 0; j < found_count; j++)
        {
            if (strcmp(found[j].text, g_vru.words[i].text) == 0)
                already = 1;
        }
        if (already)
            continue;

        for (j = 0; j < alternative_count; j++)
        {
            if (strstr(alternatives[j], g_vru.words[i].text) != NULL)
            {
                found[found_count++] = g_vru.words[i];
                break;
            }
        }
    }

    /* The longest match wins: "pikachu" beats "pika" when both are in the
     * list and the player said the longer one. */
    if (found_count > 1)
        qsort(found, (size_t)found_count, sizeof(found[0]), vru_longer_first);

    for (i = 0, k = 0; i < VRU_MAX_MATCHES; i++)
    {
        if (i < found_count)
        {
            matches[k++] = (uint16_t)found[i].slot;
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

    if (g_opt.verbose && found_count > 0)
        DebugMessage(M64MSG_WARNING, "VRU: heard \"%s\"", alternatives[0]);

    pthread_mutex_lock(&g_vru.lock);
    memcpy(g_vru.result_matches, matches, sizeof(matches));
    g_vru.result_count = found_count;
    g_vru.result_error = error;
    g_vru.result_ready = 1;
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
            g_vru.grammar_built = gen;
        }

        if (g_vru.recognizer == NULL)
            continue;

        if (take > 0)
            g_vru.fn.accept_waveform_s(g_vru.recognizer, block, (int)take);

        if (decode)
        {
            const char* json = g_vru.fn.final_result(g_vru.recognizer);
            if (json != NULL)
                vru_publish(json);
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

void vru_recognizer_set_options(const char* language, const char* mic_mode,
                                float gain, int verbose)
{
    g_opt.forced_language = 0;
    if (language != NULL && strcmp(language, "english") == 0)
        g_opt.forced_language = 1;
    else if (language != NULL && strcmp(language, "japanese") == 0)
        g_opt.forced_language = 2;

    g_opt.voice_activated = (mic_mode != NULL && strcmp(mic_mode, "voice-activated") == 0);
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
    pthread_mutex_unlock(&g_vru.lock);
}

/* ------------------------------------------------------------------ */
/* the emulation thread                                               */
/* ------------------------------------------------------------------ */

int vru_recognizer_talking(int port, int held)
{
    static int was_held[4];
    int talking = held;

    (void)port;

    if (!g_vru.available)
        return held;

    if (g_opt.voice_activated)
    {
        /* Not implemented as a detector yet: the player's own Z still works,
         * so this degrades to push-to-talk rather than to silence. */
        talking = held;
    }

    if (talking && !was_held[port & 3])
    {
        pthread_mutex_lock(&g_vru.lock);
        g_vru.listening = 1;
        g_vru.result_ready = 0;
        g_vru.ring_head = 0;
        g_vru.ring_fill = 0;
        pthread_mutex_unlock(&g_vru.lock);
    }
    else if (!talking && was_held[port & 3])
    {
        pthread_mutex_lock(&g_vru.lock);
        g_vru.listening = 0;
        g_vru.decode_pending = 1;
        pthread_cond_signal(&g_vru.wake);
        pthread_mutex_unlock(&g_vru.lock);
    }

    was_held[port & 3] = talking;
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
    int slot;

    if (!g_vru.started || word == NULL)
        return;

    text = vru_lookup(word, length);
    if (text == NULL)
    {
        if (!vru_decode_text(word, length, decoded, sizeof(decoded)))
        {
            if (g_opt.verbose)
                DebugMessage(M64MSG_WARNING, "VRU: word %d is not in the table", g_vru.word_count);
            /* Still occupies a slot: the game counts them. */
            pthread_mutex_lock(&g_vru.lock);
            g_vru.word_count++;
            pthread_mutex_unlock(&g_vru.lock);
            return;
        }
        text = decoded;
    }

    pthread_mutex_lock(&g_vru.lock);
    slot = g_vru.word_count;
    if (slot < VRU_MAX_WORDS)
    {
        snprintf(g_vru.words[slot].text, sizeof(g_vru.words[slot].text), "%s", text);
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

EXPORT void CALL vru_SetMicState(int state)
{
    if (!g_vru.started)
        return;

    pthread_mutex_lock(&g_vru.lock);
    if (!state)
    {
        g_vru.listening = 0;
        g_vru.ring_fill = 0;
        g_vru.ring_head = 0;
    }
    pthread_mutex_unlock(&g_vru.lock);
}

EXPORT void CALL vru_ReadVRUResults(uint16_t* error_flags, uint16_t* num_results,
                                    uint16_t* mic_level, uint16_t* voice_level,
                                    uint16_t* voice_length, uint16_t* matches)
{
    int waited = 0;
    int ready = 0;
    int i;

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

    /* The game asks the moment it stops listening, so give the worker the tail
     * of the utterance rather than reporting silence. */
    for (;;)
    {
        pthread_mutex_lock(&g_vru.lock);
        ready = g_vru.result_ready;
        pthread_mutex_unlock(&g_vru.lock);
        if (ready || waited >= VRU_RESULT_WAIT_MS)
            break;
        retro_sleep(10);
        waited += 10;
    }

    if (!ready)
        return;

    pthread_mutex_lock(&g_vru.lock);
    if (matches != NULL)
        memcpy(matches, g_vru.result_matches, sizeof(g_vru.result_matches));
    if (num_results != NULL)
        *num_results = (uint16_t)g_vru.result_count;
    if (error_flags != NULL && g_vru.result_error != 0)
        *error_flags = (uint16_t)g_vru.result_error;
    g_vru.result_ready = 0;
    pthread_mutex_unlock(&g_vru.lock);
}

EXPORT void CALL vru_SetVRUWordMask(uint8_t length, uint8_t* mask)
{
    /* The core has never called this. */
    (void)length;
    (void)mask;
}
