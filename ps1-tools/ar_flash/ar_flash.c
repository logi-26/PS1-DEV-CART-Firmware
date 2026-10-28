#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <psxgpu.h>
#include <psxetc.h>
#include <psxapi.h>
#include "psyq_compat.h"
#include "devcart_ui.h"

uint16_t	chip_id;

#define SCREEN_WIDTH	320
#define SCREEN_HEIGHT	240

// Screen (devcart_ui.h): one buffer, drawn again after each step (never while flashing: interrupts off)
#define	LOG_LINES	12

static DRAWENV		draw[2];
static int			fnt_log, log_count;
static char			log_text[LOG_LINES][40];
static const char	*title_right = "", *help;
static int			result;		// 0: working, 1: good, -1: error

static void	Redraw(void)
{
	int	i;

	UiTitle("PS1 DEV CART FLASHER", title_right);
	UiTextPanel(8, 20, 304, LOG_LINES);
	if (result)
		UiTile(8, 20 + UI_PANEL_H(LOG_LINES) + 6, 304, 6, (result > 0) ? 80 : 230, (result > 0) ? 220 : 70, (result > 0) ? 80 : 70);
	for (i = 0; i < log_count; i++)
		FntPrint(fnt_log, "%s\n", log_text[i]);
	UiHelp(help);
	PutDrawEnv(&draw[0]);	// (clears the screen)
	UiFlush();
	FntFlush(fnt_log);
	DrawSync(0);
}

void	InitDisplay(void)
{
	DISPENV	disp;

	InitDisplayPAL(&disp, SCREEN_WIDTH, SCREEN_HEIGHT);
	SetDefDrawEnv(&draw[0], 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	draw[1] = draw[0];
	FntLoad(960, 256);
	UiInit(draw);
	fnt_log = UiOpenText(8, 20, 304, LOG_LINES);
	log_count = 0;
	result = 0;
	help = NULL;
}

// A line of the log (x, y: from the previous version of the flasher, the lines just follow each other)
void	DrawText(int x, int y, char *text)
{
	(void)x;
	(void)y;
	if (log_count == LOG_LINES)
	{
		memmove(log_text[0], log_text[1], sizeof(log_text[0]) * (LOG_LINES - 1));
		log_count--;
	}
	strncpy(log_text[log_count], text, sizeof(log_text[0]) - 1);
	log_text[log_count++][sizeof(log_text[0]) - 1] = 0;
	Redraw();
}

void	htoa32(uint32_t n, char *strbuffer)
{
	uint8_t	i = 8;

	strbuffer = &strbuffer[8];
	*strbuffer-- = '\0';
	while (i--)
	{
		*strbuffer-- = ((n & 0xF) < 10) ? (n & 0xF) + '0' : ((n & 0xF) - 10) + 'A';
		n >>= 4;
	}
}

/****************************************/

extern uint8_t _binary_ps1devcart_rom_start[];

char	strbuffer[16];

// "ps1transfer -flash rom.rom" uploads the ROM to flash in RAM with this header before executing the
// flasher, otherwise the ROM built in the flasher is used (CD image, ps1transfer -e). The magic is cleared once read,
// so an old image left in RAM is never flashed by a later run. Status lines go to the computer (printf), ending with
// an EOT character (ps1transfer stops reading).
#define	ROM_IMAGE_ADRS	0x80100000
#define	ROM_SIZE		(128*1024)
#define	EOT				4

typedef struct
{
	char		magic[16];	// "PS1 DEV CART ROM"
	uint32_t	size;		// ROM_SIZE
	uint32_t	sum;		// Sum of the ROM bytes
	uint32_t	pad[2];
	uint8_t		data[ROM_SIZE];
} RomImage;

static uint8_t	*UploadedRom(void)
{
	RomImage	*img = (RomImage *)ROM_IMAGE_ADRS;
	uint32_t	sum = 0;
	int			i;

	if (memcmp(img->magic, "PS1 DEV CART ROM", 16) || (img->size != ROM_SIZE))
		return (NULL);
	memset(img->magic, 0, sizeof(img->magic));
	for (i = 0; i < ROM_SIZE; i++)
		sum += img->data[i];
	return ((sum == img->sum) ? img->data : NULL);
}

int main(void)
{
	int	i, size, y = 0;
	volatile uint8_t *arptr;
	uint8_t *ptr, *rom = UploadedRom();
	bool	uploaded = (rom != NULL);

	if (!rom)
		rom = _binary_ps1devcart_rom_start;
	retry:
	y = 0;	// Text from the top again after a retry (it went down off the screen)
	InitDisplay();	// (first: PSn00bSDK's ResetGraph sets up the callbacks, see psyq_compat.h)
	PadInit(0);
	title_right = uploaded ? "ROM FROM PS1TRANSFER" : "BUILT-IN ROM";

	DrawText(0, y += 8, uploaded ? "ROM uploaded by ps1transfer" : "ROM built in the flasher");
	printf("FLASH: %s\n", uploaded ? "ROM uploaded by ps1transfer" : "ROM built in the flasher");

	// From a CD (or an optical drive emulator in the parallel port, swapped for the cartridge once booted): nothing on
	// the parallel port is touched before START, released first
	if (!uploaded)
	{
		help = "START: FLASH";
		DrawText(0, y += 8, "Insert the cartridge, switch ON,");
		DrawText(0, y += 8, "then press START");
		while (PadRead(1) & PADstart)
			VSync(0);
		while (!(PadRead(1) & PADstart))
			VSync(0);
		help = NULL;
	}
	DrawText(0, y += 8, "Checking Flash Chip ID...");

	EnterCriticalSection();
	*((volatile uint8_t *)0x1F005555) = 0xAA;	// Enter ID report
	*((volatile uint8_t *)0x1F002AAA) = 0x55;
	*((volatile uint8_t *)0x1F005555) = 0x90;
	DelayUs(1000);	// (was VSync(0): PSn00bSDK's VSync waits for the VBlank interrupt, off in a critical section)
	chip_id = *((volatile uint16_t *)0x1F000000);	// Read Chip ID
	*((volatile uint8_t *)0x1F005555) = 0xAA;	// Exit ID report
	*((volatile uint8_t *)0x1F002AAA) = 0x55;
	*((volatile uint8_t *)0x1F005555) = 0xF0;
	ExitCriticalSection();

	if (chip_id != 0xD5BF)	// Verify that we have a SST39VF010 chip
	{
		title_right = "ERROR";
		result = -1;
		DrawText(0, y += 8, "Flash Chip ID is Wrong !");
		printf("FLASH ERROR: wrong flash chip ID %04X, nothing written\n%c", chip_id, EOT);
		if (!uploaded)	// (e.g. the cartridge not in yet, or switched off: nothing was written)
		{
			DrawText(0, y += 8, "Nothing written, try again");
			for (i = 0; i < 150; i++)	// (seen before the screen starts again)
				VSync(0);
			goto retry;
		}
		while (1);
	}

#ifndef DRYRUN	// Test build: nothing is written, the flash is only compared with the ROM
	DrawText(0, y += 8, "Erasing Chip...");
	printf("FLASH: erasing\n");

	EnterCriticalSection();
	*((volatile uint8_t *)0x1F005555) = 0xAA;	// Chip Erase
	*((volatile uint8_t *)0x1F002AAA) = 0x55;
	*((volatile uint8_t *)0x1F005555) = 0x80;
	*((volatile uint8_t *)0x1F005555) = 0xAA;
	*((volatile uint8_t *)0x1F002AAA) = 0x55;
	*((volatile uint8_t *)0x1F005555) = 0x10;
	ExitCriticalSection();
	for (i = 0; i < 10; i++)	// at least 100 ms
		VSync(0);

	DrawText(0, y += 8, "Flashing Chip...");
	printf("FLASH: writing\n");

	// Program Chip by page of 128 bytes
	arptr = (volatile uint8_t *)0x1F000000;
	ptr = rom;
	size = 128*1024;
	EnterCriticalSection();
	SetRCnt(RCntCNT2, 0xFFFF, RCntMdNOINTR);
	while (size > 0)
	{
		*((volatile uint8_t *)0x1F005555) = 0xAA;
		*((volatile uint8_t *)0x1F002AAA) = 0x55;
		*((volatile uint8_t *)0x1F005555) = 0xA0;
		*arptr++ = *ptr++;

		// Byte program time = 20 us (max), 30 us waited
		// RCntCNT2 = SysClock (33868800 Hz) with the BIOS SetRCnt used by PSn00bSDK (PsyQ's selected
		// SysClock / 8, 100 ticks = 23.6 us): the PSn00bSDK flasher waited only 3 us and every other byte failed
		ResetRCnt(RCntCNT2);
		while (GetRCnt(RCntCNT2) < 30 * RCNT2_TICKS_PER_US);

		size--;
	}
	ExitCriticalSection();
#endif

	DrawText(0, y += 8, "Flashing Done !");
	DrawText(0, y += 8, "Verifying...");

	arptr = (volatile uint8_t *)0x1F000000;
	ptr = rom;
	size = 128*1024;
	while (size--)
	{
		if (*arptr++ != *ptr++)
		{
			title_right = "ERROR";
			result = -1;
			help = "CROSS: RETRY";
			DrawText(0, y += 8, "Flash Error !");
			htoa32(((uint32_t)arptr)-1, strbuffer);
			DrawText(0, y += 8, strbuffer);
			DrawText(0, y += 8, "Press X to retry !");
			printf("FLASH ERROR: verify failed at %s, press X on the PS1 to retry\n%c", strbuffer, EOT);
			while (!(PadRead(1) & PADRdown))
				VSync(0);	// (the BIOS reads the pad once per frame)
			goto retry;
		}
	}
	title_right = "DONE";
	result = 1;
	DrawText(0, y += 8, "Flashing is Good !");
	printf("FLASH OK\n%c", EOT);

	while (1);

    return (0);
}
