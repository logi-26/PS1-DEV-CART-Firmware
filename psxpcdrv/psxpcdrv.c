/*
* PS1USB libsn_usb by OrionSoft [2024]
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

/*
* PS1 DEV CART psxpcdrv (named libsn_usb before): see psxpcdrv.h
* Each function is one request to ps1transfer, sent with the USB service of the kernel hook (pointer in the kernel
* hook params): 'I' init, 'O' open, 'C' create, 'S' seek, 'R' read, 'W' write, 'c' close.
*/

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "psxpcdrv.h"

typedef struct
{
	bool	is_enabled;
	int		(* p_USB_Process)(uint8_t cmd, uint32_t param, uint8_t *data, uint32_t size, uint32_t *ret);
} KernelHookParams;

// Kernel hook params are defined at the end of available reserved kernel space
// The address can be decreased to get more room if needed
// The linker script in the kernel_hook folder would need to be adjusted too
static KernelHookParams	*kernel_hook_params = (KernelHookParams *)0x8000DF70;

/****************************************/

// (no C library needed: PsyQo's toolchain has none)
static uint32_t	NameSize(const char *name)
{
	uint32_t	n = 0;

	while (name[n])
		n++;
	return (n + 1);
}

int	PCinit(void)
{
	if (!kernel_hook_params->p_USB_Process('I', 0, NULL, 0, NULL))
		return (-1);
	return (0);
}

// The name is sent with its terminating 0, the mode in the request parameter
int	PCopen(char *name, int mode, int perms)
{
	int	ret;

	if (!kernel_hook_params->p_USB_Process('O', mode, name, NameSize(name), (uint32_t *)&ret))
		return (-1);
	return (ret);
}

int	PCcreat(char *name, int perms)
{
	int	ret;

	if (!kernel_hook_params->p_USB_Process('C', 0, name, NameSize(name), (uint32_t *)&ret))
		return (-1);
	return (ret);
}

// Origin in the top byte of the parameter, file number below, offset in the size field
int	PClseek(int fd, int offset, int origin)
{
	int	ret;

	if (fd < 0)
		return (-1);
	if (!kernel_hook_params->p_USB_Process('S', (origin << 24) | (fd & 0xFFFFFF), NULL, offset, (uint32_t *)&ret))
		return (-1);
	return (ret);
}

int	PCread(int fd, char *buf, int len)
{
	int	ret;

	if (fd < 0)
		return (-1);
	if (!kernel_hook_params->p_USB_Process('R', fd, buf, len, (uint32_t *)&ret))
		return (-1);
	return (ret);
}

int	PCwrite(int fd, char *buf, int len)
{
	int	ret;

	if (fd < 0)
		return (-1);
	if (!kernel_hook_params->p_USB_Process('W', fd, buf, len, (uint32_t *)&ret))
		return (-1);
	return (ret);
}

int	PCclose(int fd)
{
	int	ret;

	if (fd < 0)
		return (-1);
	if (!kernel_hook_params->p_USB_Process('c', fd, NULL, 0, (uint32_t *)&ret))
		return (-1);
	return (ret);
}
