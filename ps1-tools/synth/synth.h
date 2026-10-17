/*
* PS1 DEV CART SPU synth: definitions shared by the PS1 program (synth.c) and ps1transfer ("-synth")
*
* The computer talks to the running synth through a mailbox at a fixed RAM address, with the memory access of the
* kernel hook ('D'/'U' while a program runs, PS1 DEV CART firmware):
* - MIDI ring: the computer writes complete MIDI messages (4 bytes each), then the head index; the synth reads them
*   up to the head and moves the tail.
* - Command: the computer writes the command fields, then increments cmd_seq; the synth runs it and sets done_seq
*   (and result). A sample is first written to the staging area.
* - State: slot table, SPU memory, counters, read by the computer.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _SYNTH_H_
#define _SYNTH_H_

#include <stdint.h>

#define	SYNTH_MAILBOX_ADRS	0x80100000
#define	SYNTH_STAGING_ADRS	0x80110000	// Sample (VAG data without its header) before it is sent to the SPU RAM
#define	SYNTH_STAGING_SIZE	0x80000		// SPU RAM is 512KB
#define	SYNTH_MAGIC			0x544E5953	// "SYNT"
#define	SYNTH_VERSION		1

#define	SYNTH_SLOTS			16			// Slot n plays on MIDI channel n (1-16)
#define	SYNTH_MIDI_RING		256			// MIDI messages (power of 2)
#define	SYNTH_NAME_LEN		16

// Commands
#define	SYNTH_CMD_LOAD		1	// Sample in the staging area -> slot (size, rate, name)
#define	SYNTH_CMD_CLEAR		2	// Free the slot
#define	SYNTH_CMD_SET		3	// Slot parameter (param, value)
#define	SYNTH_CMD_RESET		4	// All notes off, all slots cleared, default parameters

// Slot parameters (SYNTH_CMD_SET param, SynthSlot.param[])
enum
{
	SYNTH_ROOT,			// MIDI note played at the sample rate (default 60, C4)
	SYNTH_VOLUME,		// 0-127
	SYNTH_PAN,			// 0-127 (64: centre)
	SYNTH_ATTACK,		// 0-99 (0: fastest)
	SYNTH_DECAY,		// 0-99
	SYNTH_SUSTAIN,		// 0-99 (level)
	SYNTH_RELEASE,		// 0-99
	SYNTH_REVERB,		// 0/1
	SYNTH_TUNE,			// Fine tune, 0-100 (50: none), cents - 50
	SYNTH_PARAMS
};

// Command results
#define	SYNTH_OK			0
#define	SYNTH_ERR_SLOT		-1		// Bad slot number
#define	SYNTH_ERR_MEMORY	-2		// Not enough SPU RAM
#define	SYNTH_ERR_PARAM		-3

typedef struct
{
	char		name[SYNTH_NAME_LEN];	// Empty: no sample
	uint32_t	spu_adrs;				// SPU RAM address of the sample, 0: none
	uint32_t	size;					// Bytes (ADPCM)
	uint32_t	rate;					// Sample rate (Hz)
	uint8_t		param[SYNTH_PARAMS];
	uint8_t		playing;				// Voices playing it (for the display)
	uint8_t		loop;					// The sample loops (found in its ADPCM block flags)
	uint8_t		pad;
} SynthSlot;

typedef struct
{
	uint32_t	magic;
	uint32_t	version;
	// MIDI ring
	uint32_t	midi_head;				// Written by the computer
	uint32_t	midi_tail;				// Written by the synth
	uint8_t		midi[SYNTH_MIDI_RING][4];
	// Command
	uint32_t	cmd_seq;				// Incremented by the computer once the fields below are written
	uint32_t	done_seq;				// = cmd_seq when the synth has run the command
	int32_t		result;					// SYNTH_OK or SYNTH_ERR_*
	uint32_t	cmd, slot, size, rate, param, value;
	char		name[SYNTH_NAME_LEN];
	// State
	uint32_t	spu_free;				// SPU RAM free for samples (bytes)
	uint32_t	spu_total;
	uint32_t	midi_count;				// MIDI messages played
	SynthSlot	slots[SYNTH_SLOTS];
} SynthMailbox;

#endif
