/*
* PS1 DEV CART Transfer tool: file server (PCDRV): the PS1 program's files on the computer, keyboard input (from main.c)
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

FILE	*PCdrvFD[PCDRV_MAX_FD];

static PcdrvFile	pcdrv_files[PCDRV_MAX_FD];
bool			pcdrv_log, pcdrv_any_path;
int			console_last_char = '\n';	// Last character of the program's text shown

static void	PcdrvLog(const char *fmt, ...)
{
	va_list	args;

	if (console_last_char != '\n')		// (the program's text stopped in the middle of a line)
		putchar('\n');
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
	console_last_char = '\n';
}

// Only names in the served folder (no absolute path, drive or "..") unless -pcdrvany: a program can't read or write
// the other files of the computer
static bool	PcdrvPathAllowed(const char *name)
{
	const char	*p = name;

	if (pcdrv_any_path)
		return (true);
	if (!name[0] || (name[0] == '/') || strchr(name, ':'))
		return (false);
	while (*p)
	{
		if ((p[0] == '.') && (p[1] == '.') && ((p[2] == '/') || !p[2]))
			return (false);
		p = strchr(p, '/');
		if (!p)
			break;
		p++;
	}
	return (true);
}

static uint8_t				input_ring[INPUT_RING];
static volatile uint32_t	input_head, input_tail;
static int					pcdrv_input_fd = -1;
static bool					input_started;
static volatile bool		input_ended;		// End of file on the standard input (e.g. the GUI's stop)
static bool					input_pipe;			// The standard input is a pipe (GUI), seen before the thread reads it

#ifdef _WIN32
static DWORD WINAPI	InputThread(LPVOID arg)
#else
static void	*InputThread(void *arg)
#endif
{
	int	c;

	(void)arg;
	while ((c = getchar()) != EOF)
	{
		while (input_head - input_tail >= INPUT_RING)	// (full: the program doesn't read it)
#ifdef _WIN32
			Sleep(10);
#else
			usleep(10000);
#endif
		input_ring[input_head % INPUT_RING] = (uint8_t)c;
		input_head++;
	}
	input_ended = true;
	return (0);
}

// The standard input taken by "@input" (usblink.c's stop request, -prof / -link): -1 not taken, 0 taken (its lines go
// to the program), 1 stop: the end of file of a pipe (as a pipe's end of file stops without "@input"; not a console or
// NUL). Its handle isn't looked at while the thread reads it: on Windows a PeekNamedPipe waits for that read to end
// (the profiler of a program reading "@input" stopped there).
int		PC_InputState(void)
{
	return (input_started ? (input_ended && input_pipe) : -1);
}

static void	InputStart(void)
{
	if (input_started)
		return;
	input_started = true;
#ifdef _WIN32
	input_pipe = (GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_PIPE);
	CreateThread(NULL, 0, InputThread, NULL, 0, NULL);
#else
	input_pipe = !isatty(0);	// (as usblink.c's stop request without "@input")
	{
		pthread_t	t;

		pthread_create(&t, NULL, InputThread, NULL);
		pthread_detach(t);
	}
#endif
}

static uint32_t	InputRead(uint8_t *data, uint32_t size)
{
	uint32_t	n = 0;

	while ((n < size) && (input_tail != input_head))
		data[n++] = input_ring[input_tail++ % INPUT_RING];
	return (n);
}

// Opens a file for the program: file number, or -1 (refused, not found, no free file number)
static int	PcdrvOpen(char *name, const char *fmode, const char *op, const char *mode)
{
	int	i;

	if (!strcmp(name, "@input"))
	{
		for (i = 2; (i < PCDRV_MAX_FD) && PCdrvFD[i]; i++)
			;
		if (i == PCDRV_MAX_FD)
			return (-1);
		PCdrvFD[i] = stdin;		// (a marker: read from the ring)
		pcdrv_input_fd = i;
		snprintf(pcdrv_files[i].name, sizeof(pcdrv_files[i].name), "%s", name);
		pcdrv_files[i].read = pcdrv_files[i].written = 0;
		InputStart();
		return (i);
	}
	if (!PcdrvPathAllowed(name))
	{
		PcdrvLog("PCDRV: \"%s\" refused, outside the served folder (-pcdrvany allows any path)\n", name);
		if (pcdrv_log)
			PcdrvLog("pcdrv=error op=%s mode=%s name=\"%s\" reason=\"outside the folder\"\n", op, mode, name);
		return (-1);
	}
	for (i = 2; (i < PCDRV_MAX_FD) && PCdrvFD[i]; i++)
		;
	if (i == PCDRV_MAX_FD)
	{
		if (pcdrv_log)
			PcdrvLog("pcdrv=error op=%s mode=%s name=\"%s\" reason=\"too many open files\"\n", op, mode, name);
		return (-1);
	}
	if (!(PCdrvFD[i] = fopen(name, fmode)))
	{
		if (pcdrv_log)
			PcdrvLog("pcdrv=error op=%s mode=%s name=\"%s\" reason=\"%s\"\n", op, mode, name,
				(fmode[0] == 'r') ? "not found" : "can't create");
		return (-1);
	}
	snprintf(pcdrv_files[i].name, sizeof(pcdrv_files[i].name), "%s", name);
	pcdrv_files[i].read = pcdrv_files[i].written = 0;
	if (pcdrv_log)
		PcdrvLog("pcdrv=%s fd=%d mode=%s name=\"%s\"\n", op, i, mode, name);
	return (i);
}

/****************************************/
// PC File Server

bool		mon_stop_pending;	// Debug monitor stop notification ('B' request), see gdbserver.c
uint32_t	mon_stop_regs, mon_stop_info;

// first: the command byte when it was already read (profiler), -1 otherwise
// After a DLE, its type byte read: a request of the program's file server (true), or not (a profiler packet, an
// answer). The close request 'c' is also the profiler's root counter conflict packet ('c' and 1 byte): a request's 9
// bytes end on their CRC; the bytes read to see it are given back.
bool	PC_IsFileServerRequest(uint8_t type)
{
	uint8_t	p[10];

	if (type != 'c')
		return ((type == 'I') || (type == 'O') || (type == 'C') || (type == 'S') || (type == 'R') || (type == 'W') ||
			(type == 'B'));
	p[0] = type;
	if (!USB_ReadData(p + 1, 9))
		return (false);
	USB_Unread(p + 1, 9);
	return (p[9] == CRC8_Compute(p, 9));
}

void	PC_FileServerCmd(int first)
{
	uint8_t		*data, *ptr = NULL, answer, param[1+4+4+1];
	uint32_t	data_size, odata_size;
	uint32_t	fd;
	int			i;

	data = NULL;
	if (first < 0)
	{
		if (!USB_ReadData(param, 1))
			return;
		if ((param[0] == 'q') || (param[0] == 'g') || (param[0] == 'b'))	// A late escaped answer (see USB_Process)
			return;
		first = param[0];
	}
	param[0] = first;
	if (USB_ReadData(param + 1, sizeof(param) - 1))
	{
		if (param[1+4+4] == CRC8_Compute(param, 1+4+4))
			answer = 'G';	// Good ACK
		else
			answer = 'B';	// Bad command

		fd = (param[1] << 24) | (param[2] << 16) | (param[3] << 8) | param[4];
		odata_size = data_size = (param[5] << 24) | (param[6] << 16) | (param[7] << 8) | param[8];

		// Send ACK
		if (ftdi_write_data(ftdi, &answer, 1) != 1)
			goto pcdrvout;

		if (answer == 'G')
		{
			uint8_t		*odata, crc8;
			uint32_t	lsize, usize;
			int			ret_value = -1;

			// (zeroed: a short read sends zeros after the file's bytes, not what was in the computer's memory; one more
			// byte: the end of a file name is always there)
			if ((param[0] == 'O') || (param[0] == 'C') || (param[0] == 'R') || (param[0] == 'W'))
			{
				if ((data_size > PCDRV_MAX_TRANSFER) || !(ptr = data = calloc(data_size + 1, 1)))
				{
					printf("PCDRV: transfer of %u bytes refused\n", (unsigned int)data_size);
					goto pcdrvout;
				}
			}

			// Read data
			if ((param[0] == 'O') || (param[0] == 'C') || (param[0] == 'W'))
			{
				while (data_size)
				{
					lsize = (data_size > hook_packet_size) ? hook_packet_size : data_size;
					odata = ptr;
					while (lsize)
					{
						usize = (lsize > USB_READ_SIZE) ? USB_READ_SIZE : lsize;
						if (!USB_ReadData(ptr, usize))
							goto pcdrvout;
						ptr += usize;
						lsize -= usize;
					}
					// Check CRC
					lsize = (data_size > hook_packet_size) ? hook_packet_size : data_size;
					if (USB_ReadData(&crc8, 1))
					{
						if (crc8 == CRC8_Compute(odata, lsize))
						{
							answer = 'N';	// next
							data_size -= lsize;
						}
						else
						{
							answer = 'A';	// again
							ptr = odata;
						}
					}
					else
						goto pcdrvout;
					if (ftdi_write_data(ftdi, &answer, 1) != 1)
						goto pcdrvout;
				}
			}

			// Convert path
			if ((param[0] == 'O') || (param[0] == 'C'))
				for (i = 0; data[i]; i++)
					if (data[i] == '\\')
						data[i] = '/';

			switch (param[0])	// Process command
			{
				case 'I':	// Init
					ret_value = 0;
					if (pcdrv_log)
						PcdrvLog("pcdrv=init\n");
				break;

				case 'O':	// Open file
					ret_value = PcdrvOpen((char *)data, (fd == 0) ? "rb" : ((fd == 1) ? "wb" : "rb+"), "open",
						(fd == 0) ? "read" : ((fd == 1) ? "write" : "readwrite"));
				break;

				case 'C':	// Create file
					ret_value = PcdrvOpen((char *)data, "wb+", "create", "readwrite");
				break;

				case 'S':	// Seek file
					answer = (fd >> 24) & 0xFF;
					fd &= 0xFFFFFF;

					if ((fd < PCDRV_MAX_FD) && ((int)fd != pcdrv_input_fd))
					{
						if (PCdrvFD[fd])
						{
							fseek(PCdrvFD[fd], (int)data_size, (answer == 0) ? SEEK_SET : ((answer == 1) ? SEEK_CUR : SEEK_END));
							ret_value = ftell(PCdrvFD[fd]);
						}
					}
				break;

				case 'R':	// Read file
					if ((fd < PCDRV_MAX_FD) && PCdrvFD[fd] && ((int)fd == pcdrv_input_fd))
						ret_value = InputRead(data, data_size);
					else if ((fd < PCDRV_MAX_FD) && PCdrvFD[fd])
					{
						ret_value = fread(data, 1, data_size, PCdrvFD[fd]);
						pcdrv_files[fd].read += ret_value;
					}
					// Send Data (also for a file not open: the PS1 waits for the bytes, then gets -1)
					{
						{
							while (data_size)
							{
								lsize = (data_size > hook_packet_size) ? hook_packet_size : data_size;
								odata = ptr;
								while (lsize)
								{
									usize = (lsize > USB_WRITE_SIZE) ? USB_WRITE_SIZE : lsize;
									if (ftdi_write_data(ftdi, ptr, usize) != usize)
										goto pcdrvout;
									ptr += usize;
									lsize -= usize;
								}
								// Check CRC
								lsize = (data_size > hook_packet_size) ? hook_packet_size : data_size;
								if (USB_ReadData(&crc8, 1))
								{
									if (crc8 == CRC8_Compute(odata, lsize))
									{
										answer = 'N';	// next
										data_size -= lsize;
									}
									else
									{
										answer = 'A';	// again
										ptr = odata;
									}
								}
								else
									goto pcdrvout;
								if (ftdi_write_data(ftdi, &answer, 1) != 1)
									goto pcdrvout;
							}
						}
					}
				break;

				case 'W':	// Write file
					if (fd < PCDRV_MAX_FD)
					{
						if (PCdrvFD[fd])
						{
							ret_value = fwrite(data, 1, odata_size, PCdrvFD[fd]);
							pcdrv_files[fd].written += ret_value;
						}
					}
				break;

				case 'c':	// Close file
					if (fd < PCDRV_MAX_FD)
					{
						if (PCdrvFD[fd])
						{
							ret_value = ((int)fd == pcdrv_input_fd) ? 0 : fclose(PCdrvFD[fd]);
							if ((int)fd == pcdrv_input_fd)
								pcdrv_input_fd = -1;
							PCdrvFD[fd] = NULL;
							if (pcdrv_log)
								PcdrvLog("pcdrv=close fd=%d name=\"%s\" read=%u written=%u\n", (int)fd,
									pcdrv_files[fd].name, pcdrv_files[fd].read, pcdrv_files[fd].written);
						}
					}
				break;

				case 'B':	// Debug monitor: the program has stopped (registers address, reason << 8 | ExcCode)
					mon_stop_regs = fd;
					mon_stop_info = odata_size;
					mon_stop_pending = true;
					ret_value = 0;
				break;
			}

			// Send return value
			param[0] = (ret_value >> 24) & 0xFF;
			param[1] = (ret_value >> 16) & 0xFF;
			param[2] = (ret_value >> 8) & 0xFF;
			param[3] = ret_value & 0xFF;
			ftdi_write_data(ftdi, param, 4);
		}
	}
pcdrvout:
	if (data)
		free(data);
}

void	PC_FileServer(void)
{
	PC_FileServerCmd(-1);
}

