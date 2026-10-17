/*
* PS1 DEV CART video streamer: what the video server sends to the PlayStation (through "ps1transfer -net", over the
* psxcomb USB link), and what the PlayStation sends back. Shared by videostreamer.c and video_server.py.
*
* Server -> PS1: packets [type, length low, length high, payload]
*	'I'	video: canvas width, height (the whole screen, e.g. 320x240), picture width, height (centered in the canvas:
*		only the picture is sent, without the letterbox bars), encoded width, height (the picture in whole 16x16
*		macroblocks), frames per second x 100 (2 bytes each), audio sample rate (4), channels (1), video number (1),
*		number of videos (1), name (text)
*	'V'	frame: frame number (4 bytes, from 0 at the start of the video), then a BS v2 frame (MDEC bitstream)
*	'A'	audio: SPU ADPCM blocks of 16 bytes (flags 0)
*	'T'	text shown under the name (e.g. the source and its licence)
*	'R'	reset: what was received before is dropped (new video)
*	'E'	error text
* PS1 -> server: one byte commands
*	'N'	next video, 'P' previous video
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _VIDEOSTREAMER_H_
#define _VIDEOSTREAMER_H_

#define	VS_INFO			'I'
#define	VS_FRAME		'V'
#define	VS_AUDIO		'A'
#define	VS_TEXT			'T'
#define	VS_RESET		'R'
#define	VS_ERROR		'E'
#define	VS_NEXT			'N'
#define	VS_PREVIOUS		'P'

#define	VS_HEADER		3			// Type, length (2 bytes)
#define	VS_MAX_TEXT		120
#define	VS_MAX_FRAME	32768		// Biggest frame packet (frame number + BS data)
#define	VS_BLOCK		16			// ADPCM block
#define	VS_BLOCK_SAMPLES	28

#endif
