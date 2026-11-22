/*
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

/****************************************/

void	*SystemGetFile(char *filename, int *sizeptr)
{
	FILE	*f;
	int		size;
	void	*buffer = NULL;

	if (f = fopen(filename, "rb"))
	{
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sizeptr)
			*sizeptr = size;
		if (buffer = malloc(size))
			fread(buffer, 1, size, f);
		else
			printf("Memory Allocation Error for file '%s' of size %d\n", filename, size);
		fclose(f);
	}
	else
		printf("File '%s' not found !\n", filename);

	return (buffer);
}

uint32_t	atohex(char *str)
{
	uint32_t	value = 0;

	if ((str[0] == '0') && (str[1] == 'x'))
		str = &str[2];
	while (42)
	{
		if (((*str) >= '0') && ((*str) <= '9'))
		{
			value <<= 4;
			value |= (*str) - '0';
		}
		else if (((*str) >= 'A') && ((*str) <= 'F'))
		{
			value <<= 4;
			value |= ((*str) - 'A') + 0xA;
		}
		else if (((*str) >= 'a') && ((*str) <= 'f'))
		{
			value <<= 4;
			value |= ((*str) - 'a') + 0xA;
		}
		else
			break;
		str++;
	}

	return (value);
}

int main(int argc, char *argv[])
{
	int			deviceid = 0;
	int			x, y, w, h, i, ret, cmd = CMD_UNKNOWN;
	uint32_t	start_ms;						// Upload start (its rate and time shown)
	uint32_t	size = 0, adrs = 0x80010000, *ptr32, poke_value = 0;
	char		*adrs_str = NULL;
	uint8_t		*data = NULL, *ptr;
	char		*filename = NULL, *path = NULL, *serial = NULL, *libps = NULL, *auto_script = NULL;
	char		*model = NULL, *tims[VIEWER_MAX_TIM], *symfile = NULL, *sound_files[TESTER_MAX_FILES];
	char		*timview_files[TIMVIEW_MAX_FILES];
	int			sound_count = 0, timview_count = 0, card_mode = 0, card_slot = 1;
	bool		link_test = false;
	bool		pad_test = false;	// -padtest: the control tester, with the remote pad
	char		*card_file = NULL;
	int			tim_count = 0;
	bool		ttymode = false, reset = false, list = false;
	int			tty_mode = EXEC_TTY_MODE_USB;	// Default = redirect enabled (old behaviour)
	bool		tty_given = false;				// -c given
	char		*net_host = NULL;				// -net host:port
	ProfOptions	prof = {1000, 0, false, NULL, NULL, 0, NULL};	// -prof [rate[,counter]], -json, -csv, -idle
	bool		prof_on = false, prof_fill = true;	// -nofill: no RAM fill (RAM use not measured)
	bool		tim_live = false;	// -timlive: the TIM viewer linked with the GUI (timlive.c)
	bool		snd_live = false;	// -sndlive: the sound tester linked with the GUI (sndlive.c)
	bool		mdl_live = false;	// -mdllive: the model viewer linked with the GUI (mdllive.c)
	bool		remote_pad = false;	// -pad: a controller port driven from the computer (monitor/monitor.h, EXEC_PAD)
	char		*tod_files[VIEWER_MAX_TOD];
	int			tod_count = 0;
	int			gdb_port = 0;					// -gdb: debug session with the GDB server on this port
	bool		frame_live = false;				// -frame: debug session for the frame capture (gdbserver.c)
	uint32_t	gdb_entry = 0, gdb_entry_insn = 0;
	HotFile		hot_files[HOT_MAX_FILES];			// -hot: files sent to the running program
	int			hot_count = 0;
	bool		hot_watch = false;
	char		*flash_rom = NULL;				// -flash: cartridge ROM to flash
	uint8_t		*flash_data = NULL;
	bool		synth_start = true;			// -synth: the synth must be started
	int			card_live_arg = 0;
	bool		disc_start = false;				// -disc: the disc reader started (not running yet)
	int			disc_count = 0;					// -disc: the words of the command
	char		**disc_words = NULL;
	char		*snapshot_dir = NULL;			// -snapshot: the folder
	int			saver_set[2] = {-1, -1};		// -saver: the menu's, the PicoStation screen's (-1: kept)
	int			pico_details = -1;				// -picodetails: 1 on, 0 off (-1: kept)
	char		*timing_report = "timingtest-report.txt";	// -timingtest: the report
	char		*snapshot_parts = NULL;			// -snapshot: the parts (NULL: all)				// Memory card command on a card slot (argv index)
	bool		card_start = true;				// The card tool must be started (server mode)
	char		*relay_serial = NULL;			// -relay: the other cartridge of the USB link
	char		*vlink_net = NULL;				// -vlinkhost PORT / -vlinkjoin HOST:PORT
	int			vlink_net_host = 0;
	int			xserve_port = 0;
	int			vlink_ret = 0;					// -vlink: 2 when a console's driver went (exit code)
	char		*rename_arg = NULL;			// -rename NAME, -renamerestore FILE (rename.c)
	int			rename_mode = 0;			// 1: -renameinfo, 2: -rename, 3: -renamerestore
	int			exit_code = 0;					// EXIT_FAILURE after an error (used by the GUI and scripts)

	printf("PS1 DEV CART Transfer tool v%s\n\n", PS1TRANSFER_VERSION);

	// Handle arguments list
	for (i = 1; i < argc; i++)
	{
		if (argv[i][0] == '-')
		{
			switch (argv[i][1])
			{
				case 'u':	// Upload file to specified address
					cmd = CMD_UPLOAD;
				break;

				case 'b':	// The menu boots the disc in the drive (unlocked first: burned and import discs)
					if (!strcmp(argv[i], "-bootcd"))
						cmd = CMD_BOOT_DISC;
				break;

				case 'd':	// Download file from specified address and size
					if (!strcmp(argv[i], "-disc"))	// Disc reader: the files of the disc in the drive (disclive.c)
					{
						cmd = CMD_DISC;
						// The words after it up to the next option: one command, or none: commands on the input (the GUI
						// adds -progress after it; disc paths start with "\" or a letter, never "-")
						disc_words = argv + i + 1;
						for (disc_count = 0; (i + 1 < argc) && (argv[i + 1][0] != '-'); i++)
							disc_count++;
						break;
					}
					cmd = CMD_DOWNLOAD;
				break;

				case 'e':	// Execute psexe file
					cmd = CMD_EXECUTE;
				break;

				case 'r':	// Reset the PS1
					if (!strcmp(argv[i], "-renameinfo") || !strcmp(argv[i], "-rename") ||
						!strcmp(argv[i], "-renamerestore"))	// The cartridge's name (rename.c)
					{
						rename_mode = !strcmp(argv[i], "-renameinfo") ? 1 : !strcmp(argv[i], "-rename") ? 2 : 3;
						if (rename_mode > 1)
						{
							if (i + 1 >= argc)
							{
								printf("%s needed following %s\n", (rename_mode == 2) ? "The new name" : "A backup file",
									argv[i]);
								goto usage;
							}
							rename_arg = argv[++i];
						}
						break;
					}
					if (!strcmp(argv[i], "-relay"))	// USB link relay with another cartridge (usblink.c)
					{
						if (i + 1 >= argc)
						{
							printf("Serial number of the other cartridge needed following -relay\n");
							goto usage;
						}
						relay_serial = argv[++i];
						if (!gdb_port)		// (with -gdb: the relay in the debug session)
							cmd = CMD_RELAY;
						break;
					}
					reset = true;
				break;

				case 'i':	// Cartridge state as key=value lines (GUI, scripts)
					if (!strcmp(argv[i], "-idle"))	// -prof: more waiting functions (names or addresses, commas)
					{
						if (i + 1 >= argc)
						{
							printf("Function names needed following -idle\n");
							goto usage;
						}
						prof.idle = argv[++i];
						break;
					}
					if (!strcmp(argv[i], "-info"))
						cmd = CMD_INFO;
				break;

				case 'h':	// Hot reload: -hot file[@where] (several), -watch
					if (!strcmp(argv[i], "-hot"))
					{
						if ((i + 1 >= argc) || (hot_count >= HOT_MAX_FILES))
						{
							printf("File needed following -hot (max %d)\n", HOT_MAX_FILES);
							goto usage;
						}
						hot_files[hot_count++].name = argv[++i];
						cmd = CMD_HOT;
					}
				break;

				case 'w':	// Flash Action Replay
					if (!strcmp(argv[i], "-watch"))	// Hot reload: send the files again when they change
					{
						hot_watch = true;
						break;
					}
					cmd = CMD_FLASH;
				break;

				case 'j':
					if (!strcmp(argv[i], "-json"))		// -prof: "PROF {...}" lines (GUI)
						prof.json = true;
				break;

				case 'c':	// TTY Console mode
				if (!strcmp(argv[i], "-com"))	// The cartridge on a serial port: "COM7", or its USB serial number
				{
					if (i + 1 >= argc)
					{
						printf("Port (COM7) or serial number needed following -com\n");
						goto usage;
					}
#ifdef _WIN32
					com_name = argv[++i];
#else
					printf("-com: serial ports are only supported on Windows for now\n");
					return EXIT_FAILURE;
#endif
					break;
				}
				if (!strcmp(argv[i], "-csv"))	// -prof: one line per second in a CSV file
				{
					if (i + 1 >= argc)
					{
						printf("File needed following -csv\n");
						goto usage;
					}
					prof.csv = argv[++i];
					break;
				}
				if (!strcmp(argv[i], "-crash"))	// Crash report, optional symbol file (.map or nm output)
				{
					cmd = CMD_CRASH;
					if ((i + 1 < argc) && (argv[i + 1][0] != '-'))
						symfile = argv[++i];
					break;
				}
				{
					// Mode can be attached (-coff, -csio, -clink) or the next argument (-c off, -c sio, -c link)
					char	*opt = argv[i][2] ? &argv[i][2] : ((i + 1 < argc) ? argv[i + 1] : "");
					bool	next = !argv[i][2];

					tty_mode = EXEC_TTY_MODE_USB;
					if (!strcmp(opt, "off") || !strcmp(opt, "0"))	// BIOS TTY device is kept
						tty_mode = EXEC_TTY_MODE_BIOS;
					else if (!strcmp(opt, "sio"))					// Serial port TTY driver, like the Net Yaroze
						tty_mode = EXEC_TTY_MODE_SIO;
					else if (!strcmp(opt, "link"))					// The same driver on the USB link (-relay)
						tty_mode = EXEC_TTY_MODE_LINK;
					else
						next = false;

					if (next)
						i++;

					ttymode = (tty_mode == EXEC_TTY_MODE_USB);		// Can't have console if nothing is sent
					tty_given = true;
				}
				break;

				case 'n':	// open device by serial number (stable when using multiple PS1 DEV CART devices)
					if (!strcmp(argv[i], "-nofill"))	// -prof -e: free RAM not filled (RAM use not measured)
					{
						prof_fill = false;
						break;
					}
					if (!strcmp(argv[i], "-net"))	// The program's USB link connected to a TCP server (usblink.c)
					{
						if (i + 1 >= argc)
						{
							printf("host:port needed following -net\n");
							goto usage;
						}
						net_host = argv[++i];
						if (cmd == CMD_UNKNOWN)		// (with -e: after the program is executed)
							cmd = CMD_NET;
						break;
					}
					if (i + 1 >= argc)
					{
						printf("Serial number needed following -n\n");
						goto usage;
					}
					serial = argv[i + 1];
					i++;
				break;

				case 'l':	// list connected devices
					if (!strcmp(argv[i], "-live"))	// -v: the screen as it is, also at the menu (not the VRAM saved at boot)
					{
						vram_live = true;
						break;
					}
					// Link cable tester: "-link" uses the serial port like a PsyQ game (libcomb, BIOS tty kept),
					// "-link yaroze" like a Net Yaroze game (serial port tty driver, as -c sio). The tester reads
					// the tty mode to know which one.
					if (!strcmp(argv[i], "-link"))
					{
						link_test = true;
						cmd = CMD_EXECUTE;
						reset = true;	// The PS1 only accepts a reset command while a program runs
						tty_mode = EXEC_TTY_MODE_BIOS;
						if ((i + 1 < argc) && !strcmp(argv[i + 1], "yaroze"))
						{
							tty_mode = EXEC_TTY_MODE_SIO;
							i++;
						}
						else if ((i + 1 < argc) && !strcmp(argv[i + 1], "usb"))	// USB link through this computer
						{
							tty_mode = EXEC_TTY_MODE_USB;
							i++;
						}
						else if ((i + 1 < argc) && !strcmp(argv[i + 1], "yarozeusb"))	// Net Yaroze tty: on the USB link
						{
							tty_mode = EXEC_TTY_MODE_LINK;
							i++;
						}
						ttymode = false;
						break;
					}
					list = true;
				break;

				case 'm':	// TMD model to display with the model viewer
					if (!strcmp(argv[i], "-memlive"))	// Memory of the running program on request (memlive.c)
					{
						cmd = CMD_MEMLIVE;
						break;
					}
					if (!strcmp(argv[i], "-mdllive"))	// Model viewer: commands of the GUI on the input (mdllive.c)
					{
						mdl_live = true;
						break;
					}
					if (!strcmp(argv[i], "-mclist") || !strcmp(argv[i], "-mcexport") || !strcmp(argv[i], "-mcimport") ||
						!strcmp(argv[i], "-mcdelete") || !strcmp(argv[i], "-mccopy") || !strcmp(argv[i], "-mcformat"))
					{
						// slot1/slot2 instead of an image file (or as -mccopy destination): on the card itself
						if (((i + 1 < argc) && CardSlotArg(argv[i + 1])) ||
							(!strcmp(argv[i], "-mccopy") && (i + 3 < argc) && CardSlotArg(argv[i + 3])))
						{
							card_live_arg = i;
							cmd = CMD_CARD_LIVE;
							i = argc - 1;	// Its parameters follow (give -n before it)
							break;
						}
						return (CardImageCommand(argc, argv, i));	// Memory card image tools: no PS1 needed
					}
					if (!strcmp(argv[i], "-mcbackup") || !strcmp(argv[i], "-mcrestore"))	// Memory card tool
					{
						if (i + 1 >= argc)
						{
							printf("Memory card image file needed following %s\n", argv[i]);
							goto usage;
						}
						card_mode = strcmp(argv[i], "-mcbackup") ? CARD_RESTORE : CARD_BACKUP;
						card_file = argv[++i];
						cmd = CMD_EXECUTE;
						reset = true;		// The PS1 only accepts a reset command while a program runs
						ttymode = true;		// Progress text, and PCDRV file server for the backup file
						exit_on_eot = true;	// Until the PS1 program has finished
						break;
					}
					if (i + 1 >= argc)
					{
						printf("TMD file needed following -m\n");
						goto usage;
					}
					model = argv[i + 1];
					cmd = CMD_EXECUTE;
					reset = true;	// The PS1 only accepts a reset command while a program (e.g. a previous model) runs
					i++;
				break;

				case 't':	// TIM texture for the model viewer (can be used several times)
					if (!strcmp(argv[i], "-timlive"))	// TIM viewer: commands of the GUI on the input (timlive.c)
					{
						tim_live = true;
						break;
					}
					if (!strcmp(argv[i], "-tod"))	// TOD animation for the model viewer (with -m)
					{
						if ((i + 1 >= argc) || (tod_count >= VIEWER_MAX_TOD))
						{
							printf("TOD file needed following -tod (max %d)\n", VIEWER_MAX_TOD);
							goto usage;
						}
						tod_files[tod_count++] = argv[++i];
						break;
					}
					if (!strcmp(argv[i], "-timingtest"))	// Bus timing test of the cartridge port (timingtest.c)
					{
						cmd = CMD_TIMING_TEST;
						if ((i + 1 < argc) && (argv[i + 1][0] != '-'))
							timing_report = argv[++i];
						break;
					}
					if (!strcmp(argv[i], "-tim"))	// TIM file for the TIM and VRAM viewer
					{
						if ((i + 1 >= argc) || (timview_count >= TIMVIEW_MAX_FILES))
						{
							printf("TIM file needed following -tim (max %d)\n", TIMVIEW_MAX_FILES);
							goto usage;
						}
						timview_files[timview_count++] = argv[++i];
						cmd = CMD_EXECUTE;
						reset = true;	// The PS1 only accepts a reset command while a program runs
						break;
					}
					if ((i + 1 >= argc) || (tim_count >= VIEWER_MAX_TIM))
					{
						printf("TIM file needed following -t (max %d)\n", VIEWER_MAX_TIM);
						goto usage;
					}
					tims[tim_count++] = argv[i + 1];
					i++;
				break;

				case 'y':	// Net Yaroze library (libps.exe) to upload before executing
					if (i + 1 >= argc)
					{
						printf("libps.exe file needed following -y\n");
						goto usage;
					}
					libps = argv[i + 1];
					i++;
				break;

				case 'f':	// file
					if (!strcmp(argv[i], "-frame"))	// Frame capture (debug session, commands on the input, gdbserver.c)
					{
						frame_live = true;
						cmd = CMD_EXECUTE;
						reset = true;	// The PS1 only accepts a reset command while a program runs
						break;
					}
					if (!strcmp(argv[i], "-flashcheats"))	// -flash: the ROM's cheats area as it is (not the cartridge's)
					{
						flash_keep &= ~FLASH_KEEP_CHEATS;
						break;
					}
					if (!strcmp(argv[i], "-flashall"))	// -flash: the whole ROM as it is (nothing of the cartridge's kept)
					{
						flash_keep = 0;
						break;
					}
					if (!strcmp(argv[i], "-flashbackup"))	// -flash: where the flash is saved first ("none": not saved)
					{
						if (i + 1 >= argc)
						{
							printf("-flashbackup needs a folder (or none)\n");
							goto usage;
						}
						flash_backup_dir = argv[++i];
						break;
					}
					if (!strcmp(argv[i], "-flash"))	// Flash a cartridge ROM (with ar_flash.ps-exe, readback check)
					{
						if (i + 1 >= argc)
						{
							printf("ROM file needed following -flash\n");
							goto usage;
						}
						flash_rom = argv[++i];
						cmd = CMD_FLASH_ROM;
						break;
					}
					filename = argv[i + 1];
					i++;
				break;

				case 'g':	// GDB server: -gdb [port], the program is debugged with GDB (or VS Code), see gdbserver.c
					if (!strcmp(argv[i], "-gdb"))
					{
						gdb_port = 3333;
						if ((i + 1 < argc) && isdigit((unsigned char)argv[i + 1][0]))
							gdb_port = atoi(argv[++i]);
						cmd = CMD_EXECUTE;
						reset = true;	// The PS1 only accepts a reset command while a program runs
					}
				break;

				case 'p':	// PCDRV path
					if (!strcmp(argv[i], "-picodetails"))	// on/off: the PicoStation screen's discs and cheats counts
					{
						if ((i + 1 >= argc) || (strcmp(argv[i + 1], "on") && strcmp(argv[i + 1], "off")))
						{
							printf("-picodetails needs on or off\n");
							goto usage;
						}
						pico_details = !strcmp(argv[++i], "on");
						cmd = CMD_SAVER;
						break;
					}
					if (!strcmp(argv[i], "-pad"))	// Remote pad: a port driven from the computer (-memlive, -frame)
					{
						remote_pad = true;
						break;
					}
					if (!strcmp(argv[i], "-padtest"))	// Control tester (padtest.ps-exe next to ps1transfer), remote pad on
					{
						pad_test = remote_pad = true;
						cmd = CMD_EXECUTE;
						reset = true;	// The PS1 only accepts a reset command while a program runs
						break;
					}
					if (!strcmp(argv[i], "-prof"))	// Profiler of the running program (profiler.c)
					{
						prof_on = true;
						if ((i + 1 < argc) && isdigit((unsigned char)argv[i + 1][0]))
						{
							prof.rate = atoi(argv[++i]);
							if (strchr(argv[i], ','))
								prof.counter = atoi(strchr(argv[i], ',') + 1);
						}
						if (cmd == CMD_UNKNOWN)		// (with -e: after the program is executed)
							cmd = CMD_PROF;
						break;
					}
					if (!strcmp(argv[i], "-pcdrvlog"))	// File server activity lines (GUI)
					{
						pcdrv_log = true;
						break;
					}
					if (!strcmp(argv[i], "-pcdrvany"))	// File server: any path of the computer
					{
						pcdrv_any_path = true;
						break;
					}
					if (!strcmp(argv[i], "-progress"))	// Machine readable progress lines (GUI)
					{
						machine_progress = true;
						break;
					}
					if (!strcmp(argv[i], "-peek") || !strcmp(argv[i], "-poke"))	// Memory dump / write (address or @symbol)
					{
						cmd = strcmp(argv[i], "-peek") ? CMD_POKE : CMD_PEEK;
						if ((i + 1 >= argc) || ((cmd == CMD_POKE) && (i + 2 >= argc)))
						{
							printf("%s needs an address%s\n", argv[i], (cmd == CMD_POKE) ? " and a value" : "");
							goto usage;
						}
						adrs_str = argv[++i];
						if (cmd == CMD_POKE)
							poke_value = strtoul(argv[++i], NULL, 0);
						break;
					}
					path = argv[i + 1];
					i++;
				break;

				case 'a':	// 0xAddress (hexadecimal address for download/upload) or @symbol[+offset]
					if (!strcmp(argv[i], "-auto"))	// Net Yaroze auto script: the program and its data files
					{
						if (i + 1 >= argc)
						{
							printf("Auto script needed following -auto\n");
							goto usage;
						}
						auto_script = argv[++i];
						cmd = CMD_EXECUTE;
						break;
					}
					adrs_str = argv[i + 1];
					i++;
				break;

				case 'v':	// VRAM x y w h
					// -vlink's settings (the GUI can't pass environment variables): the symmetric exchange's
					// instructions ("adrs:orig:new,...", see usblink.c VlSym), the live settings file
					// Net Play between two computers: this cartridge's half of the link, waiting for the other
					// player on a port, or joining one (usblink.c VlinkNet)
					if ((!strcmp(argv[i], "-vlinkhost") || !strcmp(argv[i], "-vlinkjoin") ||
						!strcmp(argv[i], "-vlinklobby")) && (i + 1 < argc))
					{
						vlink_net_host = !strcmp(argv[i], "-vlinkhost") ? 1 : !strcmp(argv[i], "-vlinklobby") ? 2 : 0;
						vlink_net = argv[++i];
						cmd = CMD_VLINK;
						break;
					}
					{
						// Net Play settings for the relay (usblink.c), passed as environment variables
						static const char	*opts[][2] = {{"-vlinksym", "VLINK_SYM_WORDS"}, {"-vlinktune", "VLINK_TUNE"},
							{"-vlinkname", "VLINK_NAME"}, {"-vlinkgame", "VLINK_GAME"},
							{"-vlinkauto", "VLINK_AUTO_STEPS"}, {"-vlinkhostauto", "VLINK_HOST_STEPS"}, {"-vlinkwatch", "VLINK_WATCH"}, {"-vlinkkeep", "VLINK_KEEP_STEPS"}, {"-vlinkdisc", "VLINK_DISC"},
							{"-vlinkinfo", "VLINK_INFO"}};
						static char	env[sizeof(opts) / sizeof(opts[0])][1200];
						int			k, found = -1;

						for (k = 0; k < (int)(sizeof(opts) / sizeof(opts[0])); k++)
							if (!strcmp(argv[i], opts[k][0]))
								found = k;
						if ((found >= 0) && (i + 1 < argc))
						{
							snprintf(env[found], sizeof(env[found]), "%s=%s", opts[found][1], argv[++i]);
							putenv(env[found]);
							break;
						}
					}
					if (!strcmp(argv[i], "-vlink"))	// Red Alert's virtual link cable with another cartridge (usblink.c)
					{
						if (i + 1 >= argc)
						{
							printf("Serial number of the other cartridge needed following -vlink\n");
							goto usage;
						}
						relay_serial = argv[++i];
						cmd = CMD_VLINK;
						break;
					}
					if ((i + 1 < argc) && !strcmp(argv[i + 1], "auto"))	// The screen on the TV (PS1 DEV CART tools)
					{
						vram_auto = true;
						x = y = 0;
						w = 320;
						h = 240;
						i++;
						cmd = CMD_VRAM;
						break;
					}
					if (i + 4 < argc)
					{
						x = atoi(argv[i + 1]);
						y = atoi(argv[i + 2]);
						w = atoi(argv[i + 3]);
						h = atoi(argv[i + 4]);
						i += 4;
						cmd = CMD_VRAM;

						if ((x < 0) || ((x+w) > 1024) || (y < 0) || ((y+h) > 512))
						{
							printf("VRAM coordinates must be within 1024x512 !\n");
							goto usage;
						}
					}
					else
					{
						printf("VRAM command needs x y w h parameters following -v\n");
						goto usage;
					}
				break;

				case 's':	// size (size of download in decimal)
					if (!strcmp(argv[i], "-saver"))	// -saver MENU PICO: the menu's screensavers ("off", "5,logo"...)
					{
						if ((i + 2 >= argc) || ((saver_set[0] = SaverParse(argv[i + 1])) < 0) ||
							((saver_set[1] = SaverParse(argv[i + 2])) < 0))
						{
							printf("-saver needs 2 settings (the menu's, the PicoStation screen's): off, or 3, 5 or 10 "
								"(minutes) and ,logo or ,blank (e.g. -saver 5,logo off)\n");
							goto usage;
						}
						i += 2;
						cmd = CMD_SAVER;
						break;
					}
					if (!strcmp(argv[i], "-snapshot"))	// RAM, VRAM and SPU RAM of the running program to a folder
					{
						if (i + 1 >= argc)
						{
							printf("-snapshot needs the folder to write\n");
							goto usage;
						}
						snapshot_dir = argv[++i];
						if ((i + 1 < argc) && (argv[i + 1][0] != '-'))	// Parts: ram, vram, spu ("ram,vram")
							snapshot_parts = argv[++i];
						cmd = CMD_SNAPSHOT;
						break;
					}
					if (!strcmp(argv[i], "-spu"))	// SPU RAM and registers of the running program to a file
					{
						if (i + 1 >= argc)
						{
							printf("-spu needs the file to write\n");
							goto usage;
						}
						spu_file = argv[++i];
						cmd = CMD_SPU;
						break;
					}
					if (!strcmp(argv[i], "-synth"))	// SPU synth bridge, commands on the standard input (synthbridge.c)
					{
						cmd = CMD_SYNTH;
						break;
					}
					if (!strcmp(argv[i], "-sym"))	// Symbol file (.map, .sym/nm output, .elf) for -crash, -peek, @names
					{
						if (i + 1 >= argc)
						{
							printf("Symbol file needed following -sym\n");
							goto usage;
						}
						symfile = argv[++i];
						break;
					}
					if (!strcmp(argv[i], "-slot"))	// Memory card slot for -mcbackup/-mcrestore: 1, 2, 1a-1d, 2a-2d
					{
						if ((i + 1 >= argc) || !(card_slot = CardSlotParse(argv[i + 1], false)))
						{
							printf("Slot 1 or 2 (or a multitap's slot: 1a-1d, 2a-2d) needed following -slot\n");
							goto usage;
						}
						i++;
						break;
					}
					if (!strcmp(argv[i], "-sndlive"))	// Sound tester: commands of the GUI on the input (sndlive.c)
					{
						snd_live = true;
						break;
					}
					if (!strcmp(argv[i], "-snd"))	// Sound file for the sound tester (VH, VB, VAB, SEQ, VAG)
					{
						if ((i + 1 >= argc) || (sound_count >= TESTER_MAX_FILES))
						{
							printf("Sound file needed following -snd (max %d)\n", TESTER_MAX_FILES);
							goto usage;
						}
						sound_files[sound_count++] = argv[++i];
						cmd = CMD_EXECUTE;
						reset = true;	// The PS1 only accepts a reset command while a program runs
						break;
					}
					size = atoi(argv[i + 1]);
					if (size <= 0)
					{
						printf("Wrong size '%s'\n", argv[i + 1]);
						goto usage;
					}
					i++;
				break;
				
				case 'x':	// -xserve PORT: the kernel hook's exchange served to a program on this computer (pad play)
					if (!strcmp(argv[i], "-xserve") && (i + 1 < argc))
					{
						xserve_port = atoi(argv[++i]);
						cmd = CMD_XSERVE;
					}
				break;

				case 'z':	// set device id (only relevant when using multiple PS1 DEV CART devices)
					if (i + 1 >= argc)
					{
						printf("Device id needs a number following -z\n");
						goto usage;
					}
					deviceid = atoi(argv[i + 1]);
					i++;
				break;
			}
		}
		else
			goto usage;
	}
	
	// The cartridge's name: only its USB chip, by its serial number (rename.c)
	if (rename_mode)
		return ((rename_mode == 3) ? CartRenameRestore(serial, rename_arg) :
			CartRename(serial, (rename_mode == 2) ? rename_arg : NULL));

#ifdef  _WIN32
	if (list)
	{
		USB_ListDevices();
		return (0);
	}

	if (!USB_OpenDevice(deviceid, serial))
	{
		fprintf(stderr, "Unable to open USB device.\n");
		return EXIT_FAILURE;
	}
#else
	if ((ftdi = ftdi_new()) == 0)
	{
		fprintf(stderr, "ftdi_new failed\n");
		return EXIT_FAILURE;
	}

	if (list)
	{
		USB_ListDevices();
		ftdi_free(ftdi);
		return (0);
	}

	if (!USB_OpenDevice(deviceid, serial))
	{
		ftdi_free(ftdi);
		return EXIT_FAILURE;
	}
#endif

	ftdi_tcioflush(ftdi);
#ifdef _WIN32
	// Old data (e.g. printf text of a program after a "ps1transfer -c" was stopped) would be read as the answer to a command
	USB_Purge(ftdi);
#endif
	USB_SetLatency();	// After the reset/flush, which may restore the default value
	
	// The model viewer is executed with -m, unless another viewer is given with -f
	if (model && !filename)
		filename = GetToolPath(argv[0], VIEWER_FILENAME);
	// The sound tester is executed with -snd, unless another tester is given with -f
	if (sound_count && !filename)
		filename = GetToolPath(argv[0], TESTER_FILENAME);
	// The TIM viewer is executed with -tim, unless another viewer is given with -f
	if (timview_count && !filename)
		filename = GetToolPath(argv[0], TIMVIEW_FILENAME);
	// With -c, the palette editor saves/reloads the TIM files with the file server: directory of the first one (or -p)
	if (timview_count && ttymode && !path)
	{
		char	*sep, *sep2;

		path = strdup(timview_files[0]);
		sep = strrchr(path, '/');
		sep2 = strrchr(path, '\\');
		if (sep2 > sep)
			sep = sep2;
		if (sep)
			*sep = 0;
		else
			path = NULL;
	}
	// The memory card tool is executed with -mcbackup/-mcrestore, the backup file is written by the file server
	// in the directory of the given file
	// The link cable tester is executed with -link (on both consoles), unless another program is given with -f
	if (link_test && !filename)
		filename = GetToolPath(argv[0], LINKTEST_FILENAME);
	// The control tester is executed with -padtest (the remote pad on: the Remote Controller page drives it)
	if (pad_test && !filename)
		filename = GetToolPath(argv[0], PADTEST_FILENAME);
	if (card_mode && !filename)
	{
		char	*sep, *sep2;

		filename = GetToolPath(argv[0], CARD_FILENAME);
		path = strdup(card_file);
		sep = strrchr(path, '/');
		sep2 = strrchr(path, '\\');
		if (sep2 > sep)
			sep = sep2;
		if (sep)
			*sep = 0;
		else
			path = NULL;
	}

	// Address given as @symbol: resolved after all arguments (-sym can be anywhere)
	if (adrs_str && !ParseAddress(adrs_str, &adrs, symfile))
		return EXIT_FAILURE;

	// -e with -net: printf kept on the BIOS device unless -c says otherwise (text from the PS1 in the USB FIFO would
	// be read by the bridge as the answers of the kernel hook)
	if (net_host && !tty_given)
		tty_mode = EXEC_TTY_MODE_BIOS;

	// Net Yaroze auto script: its "load" program is run, its "dload" files are uploaded first (UploadAutoData)
	if (auto_script)
	{
		if (!ReadAutoScript(auto_script, 0))
			return EXIT_FAILURE;
		if (!auto_program[0])
		{
			printf("%s: no program to run (\"local load main\" line)\n", auto_script);
			return EXIT_FAILURE;
		}
		if (!libps)
		{
			printf("A Net Yaroze program needs the Net Yaroze library: add -y libps.exe (from the Net Yaroze CD)\n");
			return EXIT_FAILURE;
		}
		filename = auto_program;
		printf("Auto script %s: %s and %d data file(s)\n", auto_script, auto_program, auto_count);
	}

	if (!filename && !reset && (cmd != CMD_CRASH) && (cmd != CMD_PEEK) && (cmd != CMD_POKE) && (cmd != CMD_HOT) &&
		(cmd != CMD_MEMLIVE) &&
		(cmd != CMD_INFO) && (cmd != CMD_FLASH_ROM) && (cmd != CMD_SYNTH) &&
		(cmd != CMD_CARD_LIVE) && (cmd != CMD_RELAY) && (cmd != CMD_VLINK) && (cmd != CMD_XSERVE) && (cmd != CMD_NET) && (cmd != CMD_PROF) && (cmd != CMD_SPU) &&
		(cmd != CMD_BOOT_DISC) && (cmd != CMD_DISC) && (cmd != CMD_SNAPSHOT) && (cmd != CMD_TIMING_TEST) &&
		(cmd != CMD_SAVER))
	{
		printf("No file specified.\n");
		goto usage;
	}

	if ((cmd == CMD_UPLOAD) || (cmd == CMD_EXECUTE) || (cmd == CMD_FLASH))
	{
		data = SystemGetFile(filename, &size);
		if (!data)
			goto usage;
		if (cmd == CMD_EXECUTE)		// Net Yaroze ECOFF executable: run as a PS-X EXE
		{
			int		exe_size;
			uint8_t	*exe = EcoffToPsxExe(data, size, &exe_size);

			if (exe)
			{
				free(data);
				data = exe;
				size = exe_size;
				if (!libps)
					printf("Note: a Net Yaroze program needs the Net Yaroze library: -y libps.exe\n");
			}
			else if ((size < 2048) || memcmp(data, "PS-X EXE", 8))
			{
				printf("'%s' is not a PS-X EXE or a Net Yaroze (ECOFF) executable\n", filename);
				goto failed;
			}
		}
	}

	// -gdb: reset only if a program runs (the menu firmware may be a test build running from RAM)
	if ((cmd == CMD_FLASH_ROM) && !(flash_data = LoadRom(flash_rom)))
		goto failed;
	if (gdb_port || frame_live || (cmd == CMD_FLASH_ROM) || (cmd == CMD_BOOT_DISC))
	{
		ps1_program_running = true;		// Kept if it can't be read (program with an older kernel hook)
		USB_DetectPacketSize();
		reset = ps1_program_running;
	}
	// -synth: kept if it already runs (unless -r), otherwise started (after a reset if another program runs)
	if (cmd == CMD_SYNTH)
	{
		ps1_program_running = true;
		USB_DetectPacketSize();
		if (!reset)
			synth_start = !ps1_program_running || !SynthRunning();
		reset = synth_start && ps1_program_running;
	}
	// Memory card on a slot: the card tool server is kept if it already runs, otherwise started
	if (cmd == CMD_CARD_LIVE)
	{
		ps1_program_running = true;
		USB_DetectPacketSize();
		if (!reset)
			card_start = !ps1_program_running || !CardServerRunning();
		reset = card_start && ps1_program_running;
	}
	// -timingtest: started from the menu (after a reset if a program runs)
	if (cmd == CMD_TIMING_TEST)
	{
		USB_DetectPacketSize();
		reset = reset || ps1_program_running;
	}
	// -disc: the disc reader is kept if it already runs (unless -r), otherwise started (after a reset if another
	// program runs)
	if (cmd == CMD_DISC)
	{
		ps1_program_running = true;
		USB_DetectPacketSize();
		if (!reset)
			disc_start = !ps1_program_running || !DiscServerRunning();
		else
			disc_start = true;
		reset = disc_start && ps1_program_running;
	}

	if (reset)
	{
		// Bug on Linux only (tested on Ubuntu 22.04 VM): sometimes an USB communication issue appears,
		// ps1transfer displays "USB error." then exits when trying to execute a ps-exe file just after
		// resetting (-r and -e options combined)
		// On Windows, it works fine, the data upload seems to wait that USB becomes ready after reset

		// The PS1 can be in the middle of a transfer for a ps1transfer that was stopped (e.g. the console of a program,
		// ended by an IDE): it goes on sending, and takes the bytes of a command as the acknowledgments of its packets
		// (each one: the packet sent again). Its data read until it is quiet (it has given up waiting for an
		// acknowledgment, about 1-2 seconds), then the reset again.
		printf("Resetting the PS1...\n");
		for (i = 0; !USB_Process('R', NULL, 0, 0); i++)
		{
			uint8_t	c;
			int		quiet = 0;
			time_t	start = time(NULL);

			if (i == 3)
				goto USB_error;
			printf("The PS1 didn't take the reset: waiting until it is quiet (a transfer stopped?)...\n");
			while ((quiet < RESET_QUIET_MS) && (time(NULL) - start < RESET_DRAIN_S))
			{
				if (USB_ReadByteNoWait(&c))
					quiet = 0;
				else
				{
					SleepMs(5);
					quiet += 5;
				}
			}
		}

#ifndef _WIN32
		// Closing and opening again the USB device seems to help but not in every case
		if ((ret = ftdi_usb_close(ftdi)) < 0)
		{
			fprintf(stderr, "Unable to close USB device: %d (%s)\n", ret, ftdi_get_error_string(ftdi));
			ftdi_free(ftdi);
			return EXIT_FAILURE;
		}

		if (!USB_OpenDevice(deviceid, serial))
		{
			ftdi_free(ftdi);
			return EXIT_FAILURE;
		}
		USB_SetLatency();

		// Wait some time for the USB communication to be ready again before executing a ps-exe file
		// Otherwise, next transfers fail
		if (cmd != CMD_UNKNOWN)
			sleep_ms(1000);
#endif
	}

	// Check if the firmware supports big packets (faster transfers)
	if (cmd != CMD_UNKNOWN)
		USB_DetectPacketSize();
	// Device ID shown on the menu (PS1 DEV CART firmware)
	if ((cmd != CMD_UNKNOWN) && !ps1_program_running && (fw_features & FEATURE_DEVICE_ID))
		USB_SendDeviceId();

	// Handle command
	ptr = data;
	switch (cmd)
	{
		case CMD_EXECUTE:
			ptr32 = (uint32_t*)&data[8+4+4+(2*4)];
			adrs = *ptr32;
			ptr = &data[2048];
			size -= 2048;
			if (libps && !UploadNetYarozeLibrary(libps, adrs, size))
				goto USB_error;
			if (auto_count && !UploadAutoData(adrs, size, libps_adrs, libps_size))	// Prints its own error
				goto failed;
			// The profiler's RAM fill (free RAM above the program) would overwrite data files loaded above it
			for (i = 0; prof_on && prof_fill && (i < auto_count); i++)
				if (auto_data[i].adrs + auto_data[i].size > adrs + size)
				{
					printf("Profiler: %s is above the program, the free RAM isn't filled (RAM use not measured)\n",
						auto_data[i].path);
					prof_fill = false;
				}
			if (model && !UploadModelData(model, tims, tim_count, tod_files, tod_count))	// Prints its own file or USB error
				goto failed;
			if (sound_count && !UploadSoundData(sound_files, sound_count))
				goto failed;
			if (timview_count && !UploadTimData(timview_files, timview_count))
				goto failed;
			if (card_mode && !UploadCardData(card_mode, card_slot, card_file))
			{
				ttymode = false;
				goto failed;
			}
			if (gdb_port || frame_live)
			{
				// The firmware must have the debug monitor (its kernel hook state holds a magic number)
				uint32_t	magic = 0;

				if (!USB_Process('D', (uint8_t *)&magic, MONITOR_STATE_ADRS, 4))
					goto USB_error;
				if (magic != MONITOR_MAGIC)
				{
					printf("-gdb needs the PS1 DEV CART firmware with the debug monitor in the cartridge.\n");
					goto failed;
				}
				// Program stopped at its entry point: break instruction there, put back by the GDB server
				gdb_entry = *(uint32_t *)&data[0x10];
				if ((gdb_entry < adrs) || (gdb_entry + 4 > adrs + size) || (gdb_entry & 3))
				{
					printf("Entry point 0x%08X outside of the program.\n", gdb_entry);
					goto failed;
				}
				memcpy(&gdb_entry_insn, ptr + gdb_entry - adrs, 4);
				*(uint32_t *)(ptr + gdb_entry - adrs) = 0x0005000D;	// break 5
				tty_mode |= EXEC_DEBUG;
			}
			printf("Uploading %u bytes to 0x%X...\n", size, adrs);
			start_ms = NowMs();
			if (USB_Process('U', ptr, adrs, size))
			{
				PrintUploadTime(size, start_ms);
				// Net Yaroze: the library start code runs first (stub), like with the boot disc
				if (libps && libps_init && !gdb_port && !frame_live)
				{
					uint32_t	stub = UploadNetYarozeStart(adrs, *(uint32_t *)&data[16]);

					if (!stub)
						goto USB_error;
					*(uint32_t *)&data[16] = stub;
				}
				printf("Executing...\n");

				// The TTY redirect choice is sent with the Execute command so it is known before the program starts
				printf("TTY: %s\n", ((tty_mode & ~EXEC_DEBUG) == EXEC_TTY_MODE_USB) ? "redirected to USB" :
					(((tty_mode & ~EXEC_DEBUG) == EXEC_TTY_MODE_SIO) ? "serial port driver" :
					(((tty_mode & ~EXEC_DEBUG) == EXEC_TTY_MODE_LINK) ? "USB link driver (-relay)" : "BIOS device kept")));
				if (prof_on && prof_fill)
					printf("Profiler: free RAM filled to measure what the program uses\n");
				if (remote_pad)
				{
					MonitorState	ms;

					if (USB_Process('D', (uint8_t *)&ms, MONITOR_STATE_ADRS, sizeof(ms)) && (ms.magic == MONITOR_MAGIC) &&
						(ms.version >= 8))
						printf("Remote pad: controller port 1 can be driven from the computer%s\n",
							(ms.version >= 9) ? " (port 2 too: padport 1)" : "");
					else
					{
						printf("Remote pad: the firmware in the cartridge is too old (flash the new one)\n");
						remote_pad = false;
					}
				}
				if (!USB_Process('E', &data[8 + 4 + 4], tty_mode | ((prof_on && prof_fill) ? EXEC_PROF_FILL : 0) |
					(remote_pad ? EXEC_PAD : 0), 15 * 4))
					goto USB_error;
				// Initial stack pointer (header s_addr + s_size, 0: the one of the firmware, unknown)
				if (*(uint32_t *)&data[0x30])
					prof.stack_top = *(uint32_t *)&data[0x30] + *(uint32_t *)&data[0x34];
				// Your executable (not a PS1 DEV CART tool): its symbol file is used automatically by -crash, -peek, @names
				if (!model && !sound_count && !timview_count && !card_mode && !link_test)
					SaveLastExe(filename);
				if (prof_on)	// Profiler of the program just started
				{
					prof.symfile = symfile;
					if (UsbProfile(&prof))
						goto failed;
					goto done;
				}
				if (net_host)	// The program's USB link connected to the server
				{
					if (UsbNet(serial ? serial : "PS1", net_host))
						goto failed;
					goto done;
				}
				if (gdb_port || frame_live)
				{
					// The program runs: only the debug monitor/kernel hook answers (2KB packets)
					ps1_program_running = true;
					usb_big_packet_size = 0;
					if (path)
						chdir(path);	// PCDRV files of the program
					if (frame_live)
						exit_code = FrameLive(gdb_entry, gdb_entry_insn);
					else
						GdbServer(gdb_port, gdb_entry, gdb_entry_insn, relay_serial);
					goto done;
				}
			}
			else
				goto USB_error;
		break;

		case CMD_PEEK:	// Hexadecimal dump with symbol names (live while a program runs with the PS1 DEV CART firmware)
		{
			uint8_t		buf[4096];
			uint32_t	j, k;

			if (!size || (size > sizeof(buf)))
				size = (size > sizeof(buf)) ? sizeof(buf) : 4;
			if (!USB_Process('D', buf, adrs, size))
				goto USB_error;
			AutoLoadSymbols(symfile);
			for (j = 0; j < size; j += 16)
			{
				Symbol	*s = FindSymbol(adrs + j);

				printf("%08X ", adrs + j);
				for (k = j; k < j + 16; k++)
					printf((k < size) ? " %02X" : "   ", buf[k]);
				printf("  ");
				for (k = j; (k < j + 16) && (k < size); k++)
					putchar(((buf[k] >= 32) && (buf[k] < 127)) ? buf[k] : '.');
				if (s)
					printf("  %s+0x%X", s->name, adrs + j - s->adrs);
				printf("\n");
			}
			if ((size == 1) || (size == 2) || (size == 4))
			{
				uint32_t	v = buf[0] | ((size > 1) ? buf[1] << 8 : 0) | ((size > 2) ? (buf[2] << 16) | ((uint32_t)buf[3] << 24) : 0);

				printf("Value: %u (0x%X), signed %d\n", v, v, (size == 1) ? (int8_t)v : ((size == 2) ? (int16_t)v : (int32_t)v));
			}
		}
		break;

		case CMD_MEMLIVE:	// Memory read/written on request while the program runs (the GUI's watch list)
			if (path)
				chdir(path);	// (its file server: PCDRV files of the program)
			if (MemLive())
				goto failed;
		break;

		case CMD_POKE:	// Write a 1/2/4 bytes little endian value
		{
			uint8_t	buf[4] = {poke_value, poke_value >> 8, poke_value >> 16, poke_value >> 24};

			if ((size != 1) && (size != 2))
				size = 4;
			printf("Writing %u (0x%X), %u byte(s) at 0x%08X\n", poke_value, poke_value, size, adrs);
			if (!USB_Process('U', buf, adrs, size))
				goto USB_error;
		}
		break;

		case CMD_UPLOAD:
		case CMD_FLASH:
			printf("Uploading %u bytes to 0x%X...\n", size, adrs);
			start_ms = NowMs();
			if (USB_Process('U', ptr, adrs, size))
			{
				PrintUploadTime(size, start_ms);
				if (cmd == CMD_FLASH)
				{
					printf("Flashing Action Replay...\n");
					if (!USB_Process('F', NULL, adrs, (size+127)/128))
						goto USB_error;
				}
			}
			else
				goto USB_error;
		break;

		case CMD_DOWNLOAD:
			data = malloc(size);
			if (!data)
			{
				printf("Memory Allocation Error to download data of size %d\n", size);
				goto usage;
			}
			ptr = data;
			printf("Downloading %u bytes from 0x%X...\n", size, adrs);
			if (USB_Process('D', ptr, adrs, size))
			{
				FILE	*f;

				f = fopen(filename, "wb");
				fwrite(data, 1, size, f);
				fclose(f);
			}
			else
				goto USB_error;
		break;

		case CMD_CRASH:
			if (!CrashReport(symfile, filename))
				goto USB_error;
		break;

		case CMD_HOT:
			if (!HotReload(hot_files, hot_count, hot_watch))
				goto failed;
		break;

		case CMD_INFO:
			PrintInfo();
		break;

		case CMD_SAVER:	// The menu saves them in the cartridge flash (when they change): those not given kept
		{
			uint8_t	set[3] = {0, 0, 0}, now[8];

			if (!(fw_features & FEATURE_SCREENSAVER))
			{
				printf("The cartridge's firmware has no screensaver settings: flash a newer one.\n");
				goto failed;
			}
			if (ps1_program_running)
			{
				printf("The screensaver settings are sent to the menu: reset the PS1 first (-r).\n");
				goto failed;
			}
			if (!USB_Process('D', now, SAVER_FLASH_ADRS, sizeof(now)))
				goto failed;
			if (!memcmp(now, "SSVR", 4))
				memcpy(set, now + 4, 3);
			if (set[2] == 0xFF)	// (saved before the options)
				set[2] = 0;
			if (saver_set[0] >= 0)
				set[0] = saver_set[0], set[1] = saver_set[1];
			if (pico_details >= 0)
				set[2] = pico_details ? (set[2] & ~OPT_NO_DETAILS) : (set[2] | OPT_NO_DETAILS);
			if (!USB_Process('S', set, 0, sizeof(set)))
				goto failed;
			printf("saver=ok\n");
		}
		break;

		case CMD_BOOT_DISC:
			// After a reset (a program was running): the menu answers a few seconds later
			for (i = 0; reset && !(fw_features & FEATURE_BOOT_DISC) && (i < 10); i++)
			{
				SleepMs(1000);
				USB_DetectPacketSize();
			}
			if (!(fw_features & FEATURE_BOOT_DISC))
			{
				printf("The firmware of this cartridge can't boot the disc: flash the latest ROM.\n");
				goto failed;
			}
			if (!USB_Process('C', NULL, 0, 0))
				goto USB_error;
			printf("The PS1 boots the disc (messages on the TV).\n");
		break;

		case CMD_FLASH_ROM:
			if (!FlashRom(flash_rom, flash_data, argv[0]))
				goto failed;
		break;

		case CMD_SYNTH:
			if (SynthBridge(argv[0], synth_start))
				goto failed;
		break;

		case CMD_CARD_LIVE:
			if (CardLiveCommand(argc, argv, card_live_arg, argv[0], card_start) != EXIT_SUCCESS)
				goto failed;
		break;

		case CMD_DISC:
			if (disc_start && !DiscServerStart(argv[0]))
				goto failed;
			if (DiscLive(disc_count, disc_words))
				goto failed;
		break;

		case CMD_SNAPSHOT:
			if (!Snapshot(snapshot_dir, snapshot_parts))
				goto failed;
		break;

		case CMD_TIMING_TEST:
			if (TimingTest(argv[0], timing_report))
				goto failed;
		break;

		case CMD_RELAY:
			if (!ps1_program_running)
			{
				printf("Start the link tester on both consoles first (-link usb).\n");
				goto failed;
			}
			if (UsbRelay(serial ? serial : "first cartridge", relay_serial))
				goto failed;
		break;

		case CMD_XSERVE:
			if (XServe(serial ? serial : "PS1", xserve_port))
				goto failed;
		break;

		case CMD_VLINK:
			vlink_ret = vlink_net ? VlinkNet(serial ? serial : "PS1", vlink_net, vlink_net_host) :
				VlinkRelay(serial ? serial : "first cartridge", relay_serial);
			if (vlink_ret == 2)		// (a console's driver gone: usb_link.py installs it again)
			{
				exit_code = 2;
				goto done;
			}
			if (vlink_ret)
				goto failed;
		break;

		case CMD_PROF:
			if (!ps1_program_running)
			{
				printf("Start a program first (or add -e -f program.ps-exe).\n");
				goto failed;
			}
			prof.symfile = symfile;
			if (UsbProfile(&prof))
				goto failed;
		break;

		case CMD_NET:
			if (!ps1_program_running)
			{
				printf("Start a program using the USB link first (or add -e -f program.ps-exe).\n");
				goto failed;
			}
			if (UsbNet(serial ? serial : "PS1", net_host))
				goto failed;
		break;

		case CMD_SPU:
		{
			// The debug monitor reads them (DMA) through its 'M' service while the program runs, or directly when the
			// program is stopped in it. File: the 512KB of SPU RAM, then the 512 bytes of SPU registers.
			MonitorState	ms;
			uint8_t			*spu = malloc(MON_SPU_RAM_SIZE + MON_SPU_REGS_SIZE);
			bool			stopped, ok;
			FILE			*f;

			if (!spu)
				goto failed;
			if (!ps1_program_running)
			{
				printf("No program runs: the SPU RAM is read while a program runs (by the debug monitor)\n");
				goto failed;
			}
			if (!USB_Process('D', (uint8_t *)&ms, MONITOR_STATE_ADRS, 16))
				goto USB_error;
			if ((ms.magic != MONITOR_MAGIC) || (ms.version < 10))
			{
				printf("The firmware in the cartridge is too old to read the SPU RAM (debug monitor version 10): "
					"flash the new one\n");
				goto failed;
			}
			stopped = (ms.reason != MON_REASON_NONE);
			printf("Reading the SPU registers and RAM (512KB)...\n");
			ok = stopped ? USB_Process(MON_CMD_SPU_READ, spu + MON_SPU_RAM_SIZE, MON_SPU_REGS, MON_SPU_REGS_SIZE) :
				HotService(MON_CMD_SPU_READ, spu + MON_SPU_RAM_SIZE, MON_SPU_REGS, MON_SPU_REGS_SIZE);
			ok = ok && (stopped ? USB_Process(MON_CMD_SPU_READ, spu, 0, MON_SPU_RAM_SIZE) :
				HotService(MON_CMD_SPU_READ, spu, 0, MON_SPU_RAM_SIZE));
			if (!ok)
				goto USB_error;
			if (!(f = fopen(spu_file, "wb")) || (fwrite(spu, 1, MON_SPU_RAM_SIZE + MON_SPU_REGS_SIZE, f) !=
				MON_SPU_RAM_SIZE + MON_SPU_REGS_SIZE))
			{
				printf("Can't write %s\n", spu_file);
				if (f)
					fclose(f);
				goto failed;
			}
			fclose(f);
			printf("SPU RAM and registers written to %s\n", spu_file);
			free(spu);
		}
		break;

		case CMD_VRAM:
			data = malloc(w*h*(sizeof(uint16_t)+3));
			if (!data)
			{
				printf("Memory Allocation Error to download VRAM image\n");
				goto usage;
			}
			ptr = data;
			if ((ps1_program_running || vram_live) && (fw_features & FEATURE_LIVE_ACCESS))
			{
				// Live screenshot of the running program, read from the GPU by the kernel hook, or by the debug monitor
				// (monitor version 2 or later, through its 'M' service, unless the program is stopped in the monitor).
				// At the menu (-live), the firmware reads it ('V')
				MonitorState	ms;
				bool			service = ps1_program_running && USB_Process('D', (uint8_t *)&ms, MONITOR_STATE_ADRS, 16) &&
										(ms.magic == MONITOR_MAGIC) && (ms.version >= 2) && (ms.reason == MON_REASON_NONE);
				bool			held = false, ok;

				// -v auto: the screen on the TV as the program tells it (the TIM viewer: held on a complete frame)
				if (vram_auto)
				{
					if (ps1_program_running && (held = TimViewScreen(&x, &y)))
						printf("Screen on the TV (TIM viewer): %d,%d\n", x, y);
					else
						printf("The screen on the TV isn't known for this program: 0,0 (320x240)\n");
				}
				printf("Downloading live VRAM from coordinate %d,%d of size %d,%d...\n", x, y, w, h);
				ok = service ? HotService('V', ptr, (x << 16) | y, (w << 16) | h) :
					USB_Process('V', ptr, (x << 16) | y, (w << 16) | h);
				if (held)
					TimViewRelease();
				if (!ok)
					goto USB_error;
				ptr += w * h * sizeof(uint16_t);
			}
			else
			{
				printf("Downloading VRAM saved at boot from coordinate %d,%d of size %d,%d...\n", x, y, w, h);
				// VRAM is saved at boot by PS1 DEV CART to RAM address 0x80020000 (e.g. last screen of a program after a reset)
				for (i = 0; i < h; i++)
				{
					printf("Downloading line %d of %d...\n", i+1, h);
					adrs = 0x80020000 + ((((y + i) * 1024) + x) * sizeof(uint16_t));
					size = w * sizeof(uint16_t);
					if (USB_Process('D', ptr, adrs, size))
						ptr += size;
					else
						goto USB_error;
				}
			}
			printf("Saving to PNG image: %s\n", filename);
			VramToPng(filename, (uint16_t *)data, w, h);
		break;

		default:
			if (!reset)
				printf("No command specified.\n");
	}

	goto done;

USB_error:
	printf("\nUSB error.\n");
failed:		// Non-zero exit code on errors
	exit_code = EXIT_FAILURE;
	goto done;

usage:
	exit_code = EXIT_FAILURE;
	if (ttymode)
		goto done;

	printf("\nUsage: ps1transfer parameters...\n");
	printf("\t-u Upload file to specified address\n");
	printf("\t-d Download file from specified address and size\n");
	printf("\t-v Download VRAM image from following parameters: x y w h\n");
	printf("\t-spu file Write the SPU RAM (512KB) then the SPU registers (512 bytes, 0x1F801C00) of the running program to file (SPU RAM Viewer)\n");
	printf("\t-live With -v at the menu: the screen as it is now (default: the VRAM saved at boot)\n");
	printf("\t   (live screenshot if a program runs, otherwise the VRAM saved at the last boot)\n");
	printf("\t-d/-u/-v also work while a program runs (memory peek/poke, live screenshots) with the PS1 DEV CART firmware\n");
	printf("\t-crash [game.map] Crash report while the crash screen is displayed: cause, registers, call stack\n");
	printf("\t   with function names from a .map, .sym (nm output) or .elf file (PS1 DEV CART firmware)\n");
	printf("\t   -f frame.png: also saves the last frame the program displayed (double buffered programs)\n");
	printf("\t-sym file Symbol file (by default the .map/.sym/.elf next to the last executable run with -e)\n");
	printf("\t-peek address [-s size] Hexadecimal dump (address in hexadecimal or @symbol[+offset])\n");
	printf("\t-poke address value [-s 1|2|4] Write a value (e.g. -poke @lives 3 -s 1)\n");
	printf("\t-e Execute psexe file (use -r if a program is already running), or a Net Yaroze ECOFF executable (with -y)\n");
	printf("\t-r Reset the PS1 (will be done first if combined with actions above)\n");
	printf("\t-f file.bin\n");
	printf("\t-a 0xAddress (hexadecimal address for download/upload)\n");
	printf("\t-s size (size of download in decimal)\n");
	printf("\t-c Switch to TTY console mode after executing command.\n");
	printf("\t-c off (or -coff) Keep the PS1 BIOS TTY device, printf/TTY is not redirected to USB\n");
	printf("\t-c sio (or -csio) Install a serial port TTY driver like the Net Yaroze boot disc (e.g. link play)\n");
	printf("\t-c link (or -clink) The same TTY driver on the USB link instead of the serial port: link play without\n");
	printf("\t   cable, through the cartridges and this computer (run the program on both consoles, then -relay)\n");
	printf("\t-p path (indicate path to PCDRV file server, require -c)\n");
	printf("\t   The program can only open files in that folder (and below); -pcdrvany allows any path\n");
	printf("\t-pcdrvlog File server activity as pcdrv=... lines (open, create, close with bytes read/written, errors)\n");
	printf("\t-z deviceid (only relevant when multiple PS1 DEV CART devices are connected via USB)\n");
	printf("\t-n serial (open the PS1 DEV CART device with this USB serial number, stable unlike -z)\n");
	printf("\t   A USB serial port with this serial number if no FTDI chip has it (e.g. a Raspberry Pi Pico)\n");
	printf("\t-com COM7 or -com serial: the cartridge on a serial port (Windows), not through FTDI's D2XX driver\n");
	printf("\t   (an FTDI chip on its virtual COM port: set its latency timer to 1 ms in Device Manager)\n");
	printf("\t-l List connected USB devices with their index and serial number (then the USB serial ports)\n");
	printf("\t-flash rom.rom Flash a cartridge ROM (ar_flash.ps-exe next to ps1transfer), then read it back to check\n");
	printf("\t   (the cartridge's cheats, device ID and settings are kept; -flashcheats: the ROM's cheats area as it is,\n"
		"\t   -flashall: the whole ROM as it is, any cartridge ROM). The flash is saved first (-flashbackup DIR or none;\n"
		"\t   default: the Manager's settings folder, flash_backups), the ROM and flasher checked in the console's RAM\n");
	printf("\t-bootcd Boot the disc in the drive (the CD drive unlocked first: burned and import discs, no modchip)\n");
	printf("\t   Same as START + SELECT on the menu. Not on Japanese consoles (licensed discs still boot).\n");
	printf("\t-saver MENU PICO The menu's and the PicoStation screen's screensavers: off, or 3, 5 or 10 (minutes) and ,logo "
		"(the bouncing logo) or ,blank (e.g. -saver 5,logo off), saved in the cartridge\n");
	printf("\t-picodetails on|off The PicoStation screen's discs and cheats counts (off: faster scrolling), saved in "
		"the cartridge\n");
	printf("\t-info Cartridge state as key=value lines (device, state: menu/running/stopped/crashed, firmware...)\n");
	printf("\t-rename NAME With -n SERIAL: renames the cartridge (its USB serial number, 1-15 letters/digits),\n");
	printf("\t   nothing else changed: its USB chip's EEPROM saved first, read back after (written back if wrong)\n");
	printf("\t-renameinfo With -n SERIAL: the name and the EEPROM as key=value lines (read only)\n");
	printf("\t-renamerestore FILE With -n SERIAL: the EEPROM written back from a backup made by -rename\n");
	printf("\t-synth SPU synth (synth.ps-exe next to ps1transfer): 16 sample slots, slot n plays on MIDI channel n.\n");
	printf("\t   Commands on its input: midi HEX..., load SLOT file.vag, clear SLOT, set SLOT PARAM VALUE, state, reset, quit\n");
	printf("\t-progress Transfer progress as progress=done/total lines (for programs using ps1transfer)\n");
	printf("\t-y libps.exe (Net Yaroze library from the Net Yaroze CD-ROM, uploaded before -e)\n");
	printf("\t-auto auto -y libps.exe Net Yaroze auto script (siocons): its program and \"dload\" data files\n");
	printf("\t-m model.tmd (display a TMD model with the model viewer, tmdviewer.ps-exe next to ps1transfer)\n");
	printf("\t-t texture.tim (texture for -m, can be used up to %d times)\n", VIEWER_MAX_TIM);
	printf("\t-tod anim.tod (TOD animation for -m, up to %d: played, paused, stepped on the PS1)\n", VIEWER_MAX_TOD);
	printf("\t-mdllive With -m: commands on the input for the PS1 Dev Cart Manager (TOD, frame, play, view; state lines)\n");
	printf("\t-memlive Memory of the running program read/written on request (PS1 Dev Cart Manager's watch list), its printf and file server meanwhile\n");
	printf("\t-padtest Run the control tester (padtest.ps-exe next to ps1transfer) with -pad: the controllers of both ports as a program sees them, real or emulated (Remote Controller page)\n");
	printf("\t-pad With -e: a controller port can be driven from the computer (-memlive \"pad\"/\"padport\" commands), for programs using the BIOS pad functions; port 2 needs the debug monitor version 9\n");
	printf("\t-frame -f game.exe Frame capture (PS1 Dev Cart Manager): the program runs in a debug session, \"capture\" on the input reads the GPU packets of a frame\n");
	printf("\t-snd file (play VH+VB/VAB banks, SEQ music, VAG samples with the sound tester, up to %d files)\n", TESTER_MAX_FILES);
	printf("\t-disc [COMMAND] Disc reader: the files of the disc in the drive (ls, get PATH OUT, track N OUT.WAV, toc;\n"
		"\t\tno command: commands on the input, see disclive.c)\n");
	printf("\t-snapshot DIR [PARTS] RAM, VRAM and SPU RAM of the running program (a game booted from its disc too) to DIR\n"
		"\t\t(PARTS: some of them, e.g. ram,vram or spu)\n");
	printf("\t-timingtest [REPORT] Test of the cartridge port's bus timings (USB errors on some consoles): a few minutes,\n"
		"\t\tfrom the menu, writes REPORT (default timingtest-report.txt)\n");
	printf("\t-sndlive With -snd: commands on the input for the PS1 Dev Cart Manager (program, note, volume, play, SEQ; state lines)\n");
	printf("\t-tim file.tim (TIM and VRAM viewer: VRAM map, overlaps, CLUTs, up to %d files)\n", TIMVIEW_MAX_FILES);
	printf("\t-timlive With -tim -c: commands on the input for the PS1 Dev Cart Manager (live palette, VRAM read)\n");
	printf("\t-mcbackup card.mcd (save the memory card to a 128KB image), -mcrestore card.mcd (write it back,\n");
	printf("\t   confirmation on the PS1), -slot 1|2 (default 1)\n");
	printf("\t-link [yaroze] Link cable tester (run it on both consoles, e.g. with -n serial): serial port used like\n");
	printf("\t   a PsyQ game (libcomb, \"sio:\"), or with \"yaroze\" like a Net Yaroze game (\"tty:\" driver, as -c sio)\n");
	printf("\t   \"usb\": no cable, the consoles are linked through their cartridges and this computer (-relay)\n");
	printf("\t-vlink SERIAL2 Red Alert Retaliation's virtual link cable between -n and SERIAL2 (ps1-linkplay/virtual)\n");
	printf("\t-relay SERIAL2 USB link relay between the cartridge of -n and SERIAL2 (both run -link usb), until stopped\n");
	printf("\t-net host:port Connects the USB link of the running program (psxcomb USB link, or -c link) to a TCP\n");
	printf("\t   server, until stopped; with -e -f program.ps-exe: executes it first (printf kept on the BIOS device)\n");
	printf("\t-prof [rate[,counter]] Profiler of the running program (with -e -f program.ps-exe: started first, its free\n");
	printf("\t   RAM filled to measure what it uses, -nofill: not filled): CPU load and the functions where the time goes\n");
	printf("\t   (symbols: .map/.elf next to the program, or -sym), GPU busy, frames, DMA, stack, RAM, SPU voices, every\n");
	printf("\t   second until stopped. rate: samples a second (default 1000), counter: root counter used (default 0)\n");
	printf("\t   -json: \"PROF {...}\" lines, -csv file: one line per second in a CSV file, -idle f1,f2: more\n");
	printf("\t   functions where the program waits (not CPU load), names or addresses as shown\n");
	printf("\t-hot file[@where] Hot reload into the running program (several -hot): file.tim (VRAM at its position,\n");
	printf("\t   or file.tim@x,y), sound.vag@0x1010 / bank.vb@0x1010 (SPU RAM), data.bin@0x80123456 (RAM)\n");
	printf("\t-watch With -hot: send each file again when it changes, and show the program printf\n");
	printf("\t-gdb [port] -f game.exe Debug with GDB/VS Code: the program is loaded stopped at its entry point,\n");
	printf("\t   GDB server on localhost:port (default 3333), breakpoints, step, watchpoint, pause (PS1 DEV CART firmware)\n");
	printf("\tMemory card images (no PS1 needed, a .bak copy is kept when an image is modified):\n");
	printf("\t   -mclist card.mcd [-icons dir]   saves with titles (and icons as PNG)\n");
	printf("\t   -mcexport card.mcd SAVE out.mcs   -mcimport card.mcd in.mcs   -mcdelete card.mcd SAVE\n");
	printf("\t   slot1 or slot2 instead of card.mcd: on the card itself, only the sectors needed are read/written\n");
	printf("\t   (with a multitap: slot1a-slot1d, slot2a-slot2d; slot1 is slot1a; -slot 1b... for -mcbackup/-mcrestore)\n");
	printf("\t   (writes confirmed on the PS1), e.g. -mclist slot1, -mcexport slot1 SAVE out.mcs, -mccopy slot1 SAVE slot2\n");
	printf("\t   -mccopy src.mcd SAVE dst.mcd   -mcformat new.mcd   (SAVE: file name or block number)\n");
	printf("\nData are secured using CRC8.\n");

done:
	if (data)
		free(data);

	// Handle TTY Console mode
	memset(PCdrvFD, 0, sizeof(PCdrvFD));
	if (path)
		chdir(path);
	// -timlive: the TIM viewer just executed takes the GUI's commands (live palette, VRAM), the file server goes on
	if (tim_live && timview_count && ftdi && !exit_code)
	{
		exit_code = TimLive();
		ttymode = false;
	}
	// -mdllive: the model viewer just executed shares its animation and view controls with the GUI
	if (mdl_live && model && ftdi && !exit_code)
	{
		exit_code = MdlLive();
		ttymode = false;
	}
	// -sndlive: the sound tester just executed shares its controls with the GUI (program, note, volume, SEQ, voices)
	if (snd_live && sound_count && ftdi && !exit_code)
	{
		exit_code = SndLive();
		ttymode = false;
	}
	// -c alone (attach): the file server needs the packet size (a query can fail while the program prints)
	for (i = 0; ttymode && ftdi && !hook_packet_detected && (i < 3); i++)
		USB_DetectPacketSize();
	if (ttymode)
		setvbuf(stdout, NULL, _IONBF, 0);	// Show console text immediately, also when redirected to a file
	while (ttymode && ftdi)	// ftdi is NULL if arguments were wrong (device not opened yet)
	{
		char	c;

		if (ftdi_read_data(ftdi, &c, 1) == 1)
		{
			// Data Link Escape (PCDRV mode)
			if (c == 16)
				PC_FileServer();
			else if ((c == CARD_EOT) && exit_on_eot)	// End of a PS1 tool (memory card backup/restore)
				break;
			else
			{
				putchar(c);	// Just echo the received char
				console_last_char = c;
			}
		}
	}

	// Memory card backup: check the file written by the file server and list the saves
	if (card_mode == CARD_BACKUP)
	{
		uint8_t	*img;
		int		isize;
		char	*name = strrchr(card_file, '/') ? strrchr(card_file, '/') + 1 : card_file;

		name = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
		if (!access(name, 0) && (img = SystemGetFile(name, &isize)))	// Only if the PS1 tool has written it
		{
			if (isize == CARD_SIZE)
			{
				printf("\nBackup written: %s (%d bytes)\n", card_file, isize);
				PrintCardDirectory(img);
			}
			else
				printf("\nBackup file has a wrong size: %d bytes\n", isize);
			free(img);
		}
	}

	if (!ftdi)	// Device not opened (wrong arguments)
		return (exit_code);

#ifndef _WIN32
	if ((ret = ftdi_usb_close(ftdi)) < 0)
	{
		fprintf(stderr, "Unable to close USB device: %d (%s)\n", ret, ftdi_get_error_string(ftdi));
		ftdi_free(ftdi);
		return EXIT_FAILURE;
	}
#endif
    ftdi_free(ftdi);

	return (exit_code);
}
