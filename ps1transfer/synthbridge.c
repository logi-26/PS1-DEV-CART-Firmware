/*
* PS1 DEV CART Transfer tool: SPU synth bridge
*
* "ps1transfer -synth" starts the synth on the PS1 (ps1-tools/synth/synth.c, synth.ps-exe next to ps1transfer) unless it
* already runs, then reads commands on its standard input, one per line (typed, or sent by the GUI):
*   midi 90 3c 64 [...]        MIDI messages in hexadecimal (note on/off, controllers, pitch bend), channel = slot
*   load SLOT file.vag [name]  sample to a slot (1-16), VAG file
*   clear SLOT                 frees a slot
*   set SLOT PARAM VALUE       root, volume, pan, attack, decay, sustain, release, reverb, tune (see synth.h)
*   reset                      all notes off, all slots cleared
*   state                      prints the slots and the SPU RAM ("slot ..." lines, then "spu ...", "end")
*   quit
* Each command except midi answers "ok" or "error <reason>". It talks to the synth through its mailbox (see synth.h)
* with the memory access of the kernel hook.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "synthbridge.h"
#include "../ps1-tools/synth/synth.h"

#define	MB_FIELD(_f_)	(SYNTH_MAILBOX_ADRS + offsetof(SynthMailbox, _f_))
#define	EXEC_TTY_BIOS	1		// The synth's printf is not sent to USB (it would be read as answers)

static const char	*param_names[SYNTH_PARAMS] = {"root", "volume", "pan", "attack", "decay", "sustain", "release",
													"reverb", "tune"};
static uint32_t		midi_head, midi_tail;	// Local copies of the ring indexes
static uint32_t		cmd_seq;

static void	SleepMs(int ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	usleep(ms * 1000);
#endif
}

static bool	Read32(uint32_t adrs, uint32_t *value)
{
	return (USB_Process('D', (uint8_t *)value, adrs, 4));
}

static bool	Write32(uint32_t adrs, uint32_t value)
{
	return (USB_Process('U', (uint8_t *)&value, adrs, 4));
}

bool	SynthRunning(void)
{
	uint32_t	magic = 0;

	return (Read32(MB_FIELD(magic), &magic) && (magic == SYNTH_MAGIC));
}

// Uploads and executes the synth (the PS1 is at the menu)
static bool	StartSynth(char *argv0)
{
	uint8_t		*exe;
	int			size, i;
	bool		ok;

	if (!(exe = SystemGetFile(GetToolPath(argv0, SYNTH_FILENAME), &size)))
		return (false);
	printf("Starting the synth...\n");
	// The magic of a previous run may still be in RAM: cleared, the synth sets it once ready
	ok = Write32(MB_FIELD(magic), 0) && USB_Process('U', exe + 2048, *(uint32_t *)&exe[0x18], size - 2048) &&
		USB_Process('E', exe + 16, EXEC_TTY_BIOS, 15 * 4);
	free(exe);
	if (!ok)
		return (false);
	ps1_program_running = true;		// Only the kernel hook answers now (2KB packets)
	usb_big_packet_size = 0;
	for (i = 0; i < 50; i++)	// Its mailbox is ready once the SPU is set up
	{
		SleepMs(100);
		if (SynthRunning())
			return (true);
	}
	printf("The synth didn't start.\n");
	return (false);
}

/****************************************/
// MIDI

static int	MidiLength(uint8_t status)
{
	switch (status & 0xF0)
	{
		case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0:
			return (3);
		case 0xC0: case 0xD0:
			return (2);
	}
	return (0);		// System messages: not sent
}

// Writes the messages (4 bytes each) in the ring, then the head
static bool	SendMidi(uint8_t *bytes, int count)
{
	uint8_t		msg[SYNTH_MIDI_RING][4];
	int			i = 0, n = 0, len, first, chunk;

	while (i < count)
	{
		len = MidiLength(bytes[i]);
		if (!len || (i + len > count))
		{
			i++;
			continue;
		}
		memset(msg[n], 0, 4);
		memcpy(msg[n], bytes + i, len);
		n++;
		i += len;
		if (n == SYNTH_MIDI_RING / 2)
			break;
	}
	if (!n)
		return (true);
	// Ring full: wait for the synth (it reads continuously)
	while (midi_head + n - midi_tail > SYNTH_MIDI_RING)
		if (!Read32(MB_FIELD(midi_tail), &midi_tail))
			return (false);
	for (i = 0; i < n; i += chunk)
	{
		first = (midi_head + i) & (SYNTH_MIDI_RING - 1);
		chunk = (n - i < SYNTH_MIDI_RING - first) ? n - i : SYNTH_MIDI_RING - first;
		if (!USB_Process('U', msg[i], MB_FIELD(midi) + first * 4, chunk * 4))
			return (false);
	}
	midi_head += n;
	return (Write32(MB_FIELD(midi_head), midi_head));
}

/****************************************/
// Commands

static const char	*ErrorText(int32_t result)
{
	switch (result)
	{
		case SYNTH_ERR_SLOT:	return ("bad slot");
		case SYNTH_ERR_MEMORY:	return ("not enough SPU RAM");
		case SYNTH_ERR_PARAM:	return ("bad parameter");
	}
	return ("unknown");
}

// Writes the command fields, then cmd_seq, and waits for done_seq
static bool	Command(uint32_t cmd, uint32_t slot, uint32_t size, uint32_t rate, uint32_t param, uint32_t value,
					const char *name)
{
	struct
	{
		uint32_t	cmd, slot, size, rate, param, value;
		char		name[SYNTH_NAME_LEN];
	}			fields;
	uint32_t	done = 0;
	int32_t		result;
	time_t		start = time(NULL);

	memset(&fields, 0, sizeof(fields));
	fields.cmd = cmd;
	fields.slot = slot;
	fields.size = size;
	fields.rate = rate;
	fields.param = param;
	fields.value = value;
	if (name)
		strncpy(fields.name, name, SYNTH_NAME_LEN - 1);
	if (!USB_Process('U', (uint8_t *)&fields, MB_FIELD(cmd), sizeof(fields)) || !Write32(MB_FIELD(cmd_seq), ++cmd_seq))
	{
		printf("error usb\n");
		return (false);
	}
	while (done != cmd_seq)
	{
		if (!Read32(MB_FIELD(done_seq), &done) || (time(NULL) - start > 10))
		{
			printf("error the synth doesn't answer\n");
			return (false);
		}
	}
	if (!Read32(MB_FIELD(result), (uint32_t *)&result))
	{
		printf("error usb\n");
		return (false);
	}
	if (result == SYNTH_OK)
		printf("ok\n");
	else
		printf("error %s\n", ErrorText(result));
	return (result == SYNTH_OK);
}

static uint32_t	BigEndian32(uint8_t *p)
{
	return ((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

// VAG file: "VAGp", version, ..., data size (big endian) at 12, sample rate at 16, name at 32, data at 48
static void	Load(int slot, char *file, char *name)
{
	uint8_t		*vag;
	int			size;
	uint32_t	data_size, rate;
	char		vag_name[SYNTH_NAME_LEN + 1], *base;

	if (!(vag = SystemGetFile(file, &size)))
	{
		printf("error file not found\n");
		return;
	}
	if ((size <= 48) || memcmp(vag, "VAGp", 4))
	{
		printf("error not a VAG file\n");
		free(vag);
		return;
	}
	data_size = BigEndian32(vag + 12);
	rate = BigEndian32(vag + 16);
	if (!data_size || (data_size > (uint32_t)size - 48))
		data_size = size - 48;
	if (data_size > SYNTH_STAGING_SIZE)
	{
		printf("error sample too big\n");
		free(vag);
		return;
	}
	if (!name)
	{
		// Name: from the VAG header, else the file name without its extension
		memcpy(vag_name, vag + 32, SYNTH_NAME_LEN);
		vag_name[SYNTH_NAME_LEN] = 0;
		if (!vag_name[0])
		{
			base = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
			base = strrchr(base, '\\') ? strrchr(base, '\\') + 1 : base;
			strncpy(vag_name, base, SYNTH_NAME_LEN);
			vag_name[SYNTH_NAME_LEN] = 0;
			if (strchr(vag_name, '.'))
				*strchr(vag_name, '.') = 0;
		}
		name = vag_name;
	}
	if (!USB_Process('U', vag + 48, SYNTH_STAGING_ADRS, data_size))
		printf("error usb\n");
	else
		Command(SYNTH_CMD_LOAD, slot - 1, data_size, rate, 0, 0, name);
	free(vag);
}

static void	State(void)
{
	SynthMailbox	mb;
	SynthSlot		*s;
	int				i, p;
	uint32_t		state_start = offsetof(SynthMailbox, spu_free);

	if (!USB_Process('D', (uint8_t *)&mb + state_start, SYNTH_MAILBOX_ADRS + state_start, sizeof(mb) - state_start))
	{
		printf("error usb\n");
		return;
	}
	for (i = 0; i < SYNTH_SLOTS; i++)
	{
		s = &mb.slots[i];
		s->name[SYNTH_NAME_LEN - 1] = 0;
		printf("slot %d size=%u rate=%u loop=%d playing=%d", i + 1, s->spu_adrs ? s->size : 0, s->rate, s->loop,
			s->playing);
		for (p = 0; p < SYNTH_PARAMS; p++)
			printf(" %s=%d", param_names[p], s->param[p]);
		printf(" name=%s\n", s->spu_adrs ? s->name : "");
	}
	printf("spu free=%u total=%u midi=%u\nend\n", mb.spu_free, mb.spu_total, mb.midi_count);
}

static int	SlotArg(char *text)
{
	int	slot = text ? atoi(text) : 0;

	if ((slot < 1) || (slot > SYNTH_SLOTS))
	{
		printf("error slot 1-%d\n", SYNTH_SLOTS);
		return (0);
	}
	return (slot);
}

int	SynthBridge(char *argv0, bool start)
{
	char		line[4096], *cmd, *arg, *rest;
	uint8_t		bytes[1024];
	int			n, slot, p;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (start && !StartSynth(argv0))
		return (1);
	ps1_program_running = true;
	usb_big_packet_size = 0;
	usb_quiet = true;
	if (!Read32(MB_FIELD(midi_head), &midi_head) || !Read32(MB_FIELD(midi_tail), &midi_tail) ||
		!Read32(MB_FIELD(done_seq), &cmd_seq))
	{
		printf("USB error.\n");
		return (1);
	}
	midi_head = midi_tail;	// Messages not read yet (e.g. from a previous session) are dropped
	Write32(MB_FIELD(midi_head), midi_head);
	printf("ready\n");

	while (fgets(line, sizeof(line), stdin))
	{
		cmd = strtok(line, " \t\r\n");
		if (!cmd)
			continue;
		if (!strcmp(cmd, "midi"))
		{
			for (n = 0; (n < sizeof(bytes)) && (arg = strtok(NULL, " \t\r\n")); n++)
				bytes[n] = strtoul(arg, NULL, 16);
			if (!SendMidi(bytes, n))
				printf("error usb\n");
		}
		else if (!strcmp(cmd, "load"))
		{
			if ((slot = SlotArg(strtok(NULL, " \t\r\n"))))
			{
				arg = strtok(NULL, "\t\r\n");	// File (may contain spaces), then an optional name after a tab
				rest = arg ? strtok(NULL, "\t\r\n") : NULL;
				if (!arg)
					printf("error file needed\n");
				else
					Load(slot, arg, rest);
			}
		}
		else if (!strcmp(cmd, "clear"))
		{
			if ((slot = SlotArg(strtok(NULL, " \t\r\n"))))
				Command(SYNTH_CMD_CLEAR, slot - 1, 0, 0, 0, 0, NULL);
		}
		else if (!strcmp(cmd, "set"))
		{
			if ((slot = SlotArg(strtok(NULL, " \t\r\n"))))
			{
				arg = strtok(NULL, " \t\r\n");
				rest = strtok(NULL, " \t\r\n");
				for (p = 0; arg && (p < SYNTH_PARAMS) && strcmp(arg, param_names[p]); p++)
					;
				if (!arg || !rest || (p >= SYNTH_PARAMS))
					printf("error set SLOT PARAM VALUE (root, volume, pan, attack, decay, sustain, release, reverb, "
						"tune)\n");
				else
					Command(SYNTH_CMD_SET, slot - 1, 0, 0, p, atoi(rest), NULL);
			}
		}
		else if (!strcmp(cmd, "reset"))
			Command(SYNTH_CMD_RESET, 0, 0, 0, 0, 0, NULL);
		else if (!strcmp(cmd, "state"))
			State();
		else if (!strcmp(cmd, "quit"))
			break;
		else
			printf("error unknown command %s\n", cmd);
	}
	return (0);
}
