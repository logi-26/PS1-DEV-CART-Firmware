/*
* PS1 DEV CART firmware: the cheat screen (Select + Triangle on the menu). The disc's cheats in the cartridge are
* listed, the ones chosen go into the code engine's table (usb.h: CheatTable), then the game boots.
*
* The cheats are in the cartridge's flash at CHEAT_DB_ROM (cartrom/rom.s), written by the computer (the Manager's
* Cheats page, gui/cheatdb.py: rom_cheats). Little endian:
*	"CHDB", version (2), games (2), size of it all (4)
*	each game: serial (12: the disc's executable, e.g. "SLES_015.42", 0 padded), title (32, 0 padded), cheats (2)
*	each cheat: name length (1), name, codes (1), each code: its first word (4: type << 24 | address), value (2)
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _CHEATMENU_H_
#define _CHEATMENU_H_

static uint32_t	RomLe(const volatile uint8_t *p, int n)
{
	uint32_t	v = 0;

	while (n--)
		v = (v << 8) | p[n];
	return (v);
}

#define	CHEAT_DB_ROM		0x1F00B000
#define	CHEAT_DB_SIZE		0x5000
#define	CHEAT_DB_MAGIC		0x42444843	// "CHDB"
#define	CHEAT_DB_SERIAL		12
#define	CHEAT_DB_TITLE		32
#define	CHEATS_SHOWN		64			// Cheats of a game listed at most
#define	CHEAT_ROWS			20			// Lines of the list
#define	CHEAT_ROW_H			10
#define	CHEAT_LIST_Y		20
#define	CHEAT_SAVE_Y		256			// The menu's picture kept there in VRAM meanwhile (Back puts it back)
#define	CHEAT_DB_RAM		0x80150000	// Test in RAM (make RAMTEST=1): the cheats uploaded there first are used

static const volatile uint8_t	*CheatDb(void)
{
#ifdef TFS_RAM_ADRS
	if (RomLe((const volatile uint8_t *)CHEAT_DB_RAM, 4) == CHEAT_DB_MAGIC)
		return ((const volatile uint8_t *)CHEAT_DB_RAM);
#endif
	return ((const volatile uint8_t *)CHEAT_DB_ROM);
}


// The game's cheats: their places in the flash (count), 0 if the game has none
static int	CheatFind(const char *serial, const volatile uint8_t **cheats, char *title)
{
	const volatile uint8_t	*db = CheatDb(), *end, *g;
	int						games, k, n, i;

	if (RomLe(db, 4) != CHEAT_DB_MAGIC)
		return (0);
	games = RomLe(db + 6, 2);
	end = db + ((RomLe(db + 8, 4) < CHEAT_DB_SIZE) ? RomLe(db + 8, 4) : CHEAT_DB_SIZE);
	g = db + 12;
	for (k = 0; (k < games) && (g + CHEAT_DB_SERIAL + CHEAT_DB_TITLE + 2 <= end); k++)
	{
		const volatile uint8_t	*c = g + CHEAT_DB_SERIAL + CHEAT_DB_TITLE + 2;
		int						count = RomLe(g + CHEAT_DB_SERIAL + CHEAT_DB_TITLE, 2), same = 1;

		for (i = 0; i < CHEAT_DB_SERIAL; i++)
		{
			if (g[i] != (uint8_t)serial[i])
				same = 0;
			if (!serial[i])
				break;
		}
		for (n = 0; (n < count) && (c + 2 <= end); n++)
		{
			if (same && (n < CHEATS_SHOWN))
				cheats[n] = c;
			c += 1 + c[0];				// Name
			c += 1 + c[0] * 6;			// Codes
		}
		if (same)
		{
			for (i = 0; i < CHEAT_DB_TITLE - 1; i++)
				title[i] = g[CHEAT_DB_SERIAL + i];
			title[i] = 0;
			return ((count < CHEATS_SHOWN) ? count : CHEATS_SHOWN);
		}
		g = c;
	}
	return (0);
}

static void	CheatDraw(const volatile uint8_t **cheats, int n, const uint8_t *on, int cursor, int top,
	const char *title)
{
	char	text[40];
	int		r, i;

	FillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, UI_BG);
	FillRect(0, 0, SCREEN_WIDTH, UI_TITLE_H, UI_BAR);
	DrawText(8, 4, title);
	for (r = 0; (r < CHEAT_ROWS) && (top + r < n); r++)
	{
		const volatile uint8_t	*c = cheats[top + r];
		int						y = CHEAT_LIST_Y + r * CHEAT_ROW_H, len = (c[0] < 32) ? c[0] : 32;	// (the arrow after)

		if (top + r == cursor)
			FillRect(4, y - 1, SCREEN_WIDTH - 8, CHEAT_ROW_H, UI_SELECT);
		DrawTextColor(8, y, on[top + r] ? "[X]" : "[ ]", TEXT_ACCENT);
		for (i = 0; i < len; i++)
			text[i] = c[1 + i];
		text[i] = 0;
		DrawText(40, y, text);
	}
	if (top + CHEAT_ROWS < n)
		DrawTextColor(SCREEN_WIDTH - 16, CHEAT_LIST_Y + (CHEAT_ROWS - 1) * CHEAT_ROW_H, "v", TEXT_DIM);
	FillRect(0, UI_HELP_Y, SCREEN_WIDTH, UI_HELP_H, UI_PANEL);
	DrawTextColor(8, UI_HELP_Y + 3, "X: ON/OFF   START: BOOT   O: BACK", TEXT_DIM);
}

// The cheat screen: 1 to boot (the cheats chosen in the code engine's table), 0 to go back to the menu
int	CheatMenu(const char *serial)
{
	static const volatile uint8_t	*cheats[CHEATS_SHOWN];
	static uint8_t					on[CHEATS_SHOWN];
	char							title[CHEAT_DB_TITLE + 8];
	RECT							screen = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
	int								n, i, cursor = 0, top = 0, held = 0, redraw = 1;
	uint32_t						pad, old;

	strcpy(title, "CHEATS: ");
	if (!(n = CheatFind(serial, cheats, title + 8)))
	{
		PrintConsole("No cheats for it in the cartridge");
		return (1);
	}
	memset(on, 0, sizeof(on));
	MoveImage(&screen, 0, CHEAT_SAVE_Y);
	DrawSync(0);
	old = PadRead(1) & 0xFFFF;				// (Select + Triangle still held: only new presses count)
	while (1)
	{
		VSync(0);
		// The computer answered meanwhile, as on the menu (e.g. a live screenshot: ps1transfer -v ... -live)
		if (!(USB_STATUS & USB_STATUS_MASK_USB_READY))
			USB_Process(0, 0, NULL, 0, NULL);
		pad = PadRead(1) & 0xFFFF;
		if (pad == 0xFFFF)					// (a bad read)
			continue;
		// Up/down: once, then repeated while held
		held = (pad & (PADLup | PADLdown)) ? held + 1 : 0;
		if (((pad & ~old) & PADLup) || ((held > 20) && !(held & 3) && (pad & PADLup)))
		{
			cursor = cursor ? cursor - 1 : n - 1;
			redraw = 1;
		}
		if (((pad & ~old) & PADLdown) || ((held > 20) && !(held & 3) && (pad & PADLdown)))
		{
			cursor = (cursor + 1 < n) ? cursor + 1 : 0;
			redraw = 1;
		}
		if ((pad & ~old) & PADRdown)
		{
			on[cursor] ^= 1;
			redraw = 1;
		}
		if ((pad & ~old) & PADstart)
			break;
		if ((pad & ~old) & PADRright)		// Back: the menu as it was
		{
			RECT	saved = {0, CHEAT_SAVE_Y, SCREEN_WIDTH, SCREEN_HEIGHT};

			MoveImage(&saved, 0, 0);
			DrawSync(0);
			return (0);
		}
		old = pad;
		if (redraw)
		{
			if (cursor < top)
				top = cursor;
			if (cursor >= top + CHEAT_ROWS)
				top = cursor - CHEAT_ROWS + 1;
			CheatDraw(cheats, n, on, cursor, top, title);
			redraw = 0;
		}
	}

	// The cheats chosen: their codes in the code engine's table
	{
		volatile CheatTable	*t = (volatile CheatTable *)CHEAT_TABLE_ADRS;
		int					count = 0, k;

		t->magic = 0;
		for (i = 0; i < n; i++)
		{
			const volatile uint8_t	*c = cheats[i] + 1 + cheats[i][0];

			if (!on[i])
				continue;
			for (k = 0; (k < c[0]) && (count < CHEAT_MAX); k++, count++)
			{
				t->codes[count].code = RomLe(c + 1 + k * 6, 4);
				t->codes[count].value = RomLe(c + 5 + k * 6, 2);
			}
		}
		t->count = count;
		if (count)
			t->magic = CHEAT_TABLE_MAGIC;
	}
	FillRect(0, UI_HELP_Y, SCREEN_WIDTH, UI_HELP_H, UI_PANEL);
	DrawTextColor(8, UI_HELP_Y + 3, "BOOTING THE DISC...", TEXT_DIM);
	return (1);
}

#endif
