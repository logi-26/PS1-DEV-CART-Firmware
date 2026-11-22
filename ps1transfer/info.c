/*
* PS1 DEV CART Transfer tool: -info and the menu's screensaver settings (from main.c)
*
* PS1USB Transfer tool v1.0
* by OrionSoft [2024]
* https://orionsoft.games/
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*
* Linux version require libftdi1-dev
*/

#include "ps1transfer.h"

/****************************************/
// "-info" prints the cartridge state as key=value lines (for the GUI and scripts)

/****************************************/
// The menu's screensavers (-saver, FEATURE_SCREENSAVER): a byte each (the menu's, the PicoStation screen's): bits 0-1
// the timeout (0: 3 minutes, 1: 5, 2: 10), bit 2 the bouncing logo (else the display off), bit 3 off. As text: "off",
// or the minutes and ",logo" / ",blank" ("5,logo"; the minutes alone: blank).

int	SaverParse(const char *text)
{
	static const int	minutes[] = {3, 5, 10};
	int					m = atoi(text), i;
	const char			*style = strchr(text, ',');

	if (!strcmp(text, "off"))
		return (SAVER_OFF);
	for (i = 0; (i < 3) && (minutes[i] != m); i++)
		;
	if ((i == 3) || (style && strcmp(style, ",logo") && strcmp(style, ",blank")))
		return (-1);
	return (i | ((style && !strcmp(style, ",logo")) ? SAVER_LOGO : 0));
}

static void	SaverText(uint8_t c, char *out)
{
	static const int	minutes[] = {3, 5, 10, 10};

	if (c & SAVER_OFF)
		strcpy(out, "off");
	else
		sprintf(out, "%d,%s", minutes[c & 3], (c & SAVER_LOGO) ? "logo" : "blank");
}

void	PrintInfoTo(FILE *out);

void	PrintInfo(void)
{
	PrintInfoTo(stdout);
}

void	PrintInfoTo(FILE *out)
{
	uint8_t		params[4], state[16];
	char		serial[64];
	bool		have_state;
	const char	*status;

	fprintf(out, "device=%s\n", USB_GetSerial(serial) ? serial : "");
	if (!USB_Process('D', params, KERNEL_HOOK_PARAMS_ADRS, sizeof(params)))
	{
		// Original firmware while a program runs, or nothing answers
		fprintf(out, "state=unknown\n");
		return;
	}
	have_state = params[3] && USB_Process('D', state, MONITOR_STATE_ADRS, sizeof(state)) &&
		(*(uint32_t *)state == MONITOR_MAGIC);
	if (!params[0])
		status = "menu";
	else if (have_state && (state[12] == MON_REASON_CRASH))
		status = "crashed";
	else if (have_state && (state[12] >= MON_REASON_BREAK) && (state[12] <= MON_REASON_PAUSE))
		status = "stopped";
	else
		status = "running";
	fprintf(out, "state=%s\n", status);
	fprintf(out, "firmware=%s\n", params[3] ? "devcart" : "original");
	if (have_state && state[15])
		fprintf(out, "firmware_version=%d.%d\n", state[15] >> 4, state[15] & 15);
	fprintf(out, "features=0x%02X\n", params[3]);
	fprintf(out, "tty_mode=%d\n", params[1] & ~EXEC_DEBUG);
	fprintf(out, "debug_session=%d\n", (params[1] & EXEC_DEBUG) ? 1 : 0);
	fprintf(out, "monitor=%d\n", (have_state && *(uint32_t *)(state + 4)) ? 1 : 0);
	fprintf(out, "monitor_version=%d\n", have_state ? state[14] : 0);
	fprintf(out, "escaped_answers=%d\n", (USB_EscapedAnswers() > 0) ? 1 : 0);
	// The screensavers' settings (at the menu: the flash read; none saved: the default, 3 minutes, display off)
	if (!params[0] && (params[3] & FEATURE_SCREENSAVER))
	{
		uint8_t	saver[8];
		char	text[2][16];

		if (USB_Process('D', saver, SAVER_FLASH_ADRS, sizeof(saver)))
		{
			if (memcmp(saver, "SSVR", 4))
				saver[4] = saver[5] = saver[6] = 0;
			SaverText(saver[4], text[0]);
			SaverText(saver[5], text[1]);
			fprintf(out, "saver_menu=%s\nsaver_pico=%s\n", text[0], text[1]);
			// The PicoStation screen's list: the discs and cheats counts shown (0xFF: saved before this option)
			fprintf(out, "pico_details=%s\n", ((saver[6] != 0xFF) && (saver[6] & OPT_NO_DETAILS)) ? "off" : "on");
		}
	}
	// The game booted from its disc: its executable (the disc's serial number, e.g. SLES_015.42)
	if (params[0] && (params[3] & FEATURE_DISC_ID))
	{
		uint8_t	id[32];

		if (USB_Process('D', id, DISC_ID_ADRS, sizeof(id)) && (*(uint32_t *)id == DISC_ID_MAGIC))
		{
			id[sizeof(id) - 1] = 0;
			fprintf(out, "game=%s\n", (char *)id + 4);
		}
	}
}

