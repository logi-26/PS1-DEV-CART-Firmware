/*
* PS1 DEV CART Transfer tool: the cartridge's name (USB serial number), see rename.c
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef _PS1TRANSFER_RENAME_H_
#define _PS1TRANSFER_RENAME_H_

// The cartridge -n SERIAL renamed (name NULL: what its EEPROM holds, read only), or its EEPROM written back from a
// backup made by the rename: the exit code
int		CartRename(const char *serial, const char *name);
int		CartRenameRestore(const char *serial, const char *file);

#endif
