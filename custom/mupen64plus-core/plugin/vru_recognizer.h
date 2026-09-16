/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - vru_recognizer.h                                        *
 *                                                                         *
 *   The speech recognizer behind the Voice Recognition Unit. Derived from  *
 *   Rosalie's Mupen GUI (Source/RMG-Input/VRU.cpp), which is GPLv3, so     *
 *   this file and vru_recognizer.c are GPLv3 or later.                     *
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

#ifndef M64P_PLUGIN_VRU_RECOGNIZER_H
#define M64P_PLUGIN_VRU_RECOGNIZER_H

#include <stdint.h>

/* Everything here is called on the thread that calls retro_run, except
 * vru_recognizer_talking, which arrives on the emulation thread and only
 * touches flags. The five plugin entry points are declared in plugin.c, the
 * way the rest of this input plugin's are. */

void vru_recognizer_init(const char* system_dir);
void vru_recognizer_close(void);

/* Opens the microphone once a port is a VRU, hands it whatever it captured,
 * and says once if the frontend asked too late for it to attach. */
void vru_recognizer_pump(void);

/* A loaded state does not carry the game's word list, so the list the
 * recognizer is holding is no longer the one the game believes in. */
void vru_recognizer_reset_words(void);

void vru_recognizer_set_options(const char* language, const char* mic_mode,
                                float gain, int verbose);

/* Z on a VRU port is not Z: the game reads it to know someone is talking, and
 * times the utterance by how long it is held. Returns what the core should
 * see, and marks the beginning and end of an utterance. */
int vru_recognizer_talking(int port, int held);

/* Said by inputInitiateControllers for each port as the joybus channels are
 * built. A port that becomes a VRU after this is one the game will never see
 * as one, which is the difference between "not working" and "reload it". */
void vru_recognizer_note_attached(int port, int is_vru);

/* For the selftest: bring the library, the model and the worker up without a
 * frontend or a microphone, then feed capture straight in. Everything after
 * that -- the word table, the grammar, the match rule -- is the path the core
 * uses, which is the point of testing it this way. */
int  vru_recognizer_start_offline(const char* system_dir);
void vru_recognizer_push_pcm(const int16_t* samples, unsigned count);
int  vru_recognizer_is_available(void);

#endif /* M64P_PLUGIN_VRU_RECOGNIZER_H */
