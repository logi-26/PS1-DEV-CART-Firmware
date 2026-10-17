/*
* PS1 DEV CART audio streamer: what the audio server sends to the PlayStation (through "ps1transfer -net", over the psxcomb
* USB link), and what the PlayStation sends back. Shared by audiostreamer.c and audio_server.py (same values there).
*
* Server -> PS1: packets [type, length low, length high, payload]
*	'I'	station or file: sample rate (4 bytes), channels (1), number in the playlist (1), playlist size (1), name (text)
*	'T'	title of the song playing (text: the stream's ICY metadata, or the file's tags)
*	'A'	audio: SPU ADPCM blocks of 16 bytes (28 samples each, flags 0: the PS1 sets the loop flags)
*	'R'	reset: the audio received before is dropped (new station or file)
*	'E'	error text (e.g. the station doesn't answer)
* PS1 -> server: one byte commands
*	'N'	next in the playlist, 'P' previous
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _AUDIOSTREAMER_H_
#define _AUDIOSTREAMER_H_

#define	AS_INFO			'I'
#define	AS_TITLE		'T'
#define	AS_AUDIO		'A'
#define	AS_RESET		'R'
#define	AS_ERROR		'E'
#define	AS_NEXT			'N'
#define	AS_PREVIOUS		'P'

#define	AS_HEADER		3			// Type, length (2 bytes)
#define	AS_MAX_TEXT		120			// Longest text payload kept
#define	AS_BLOCK		16			// ADPCM block
#define	AS_BLOCK_SAMPLES	28

#endif
