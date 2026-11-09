/*
* What the movie tester uses of PSn00bSDK and the PS1 DEV CART headers, for its host build (vthost.c defines the
* functions): types, constants, the functions' prototypes.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/
#ifndef VTHOST_SDK_H
#define VTHOST_SDK_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// (names Windows has too: the PS1 ones renamed)
#undef	RECT
#undef	LoadImage
#define	RECT		PSX_RECT
#define	LoadImage	PSX_LoadImage

typedef struct {int16_t x, y, w, h;} RECT;
typedef struct {int unused;} DISPENV;
typedef struct {int unused;} DRAWENV;
typedef struct {int unused;} POLY_FT4;
typedef struct {int unused;} DR_TPAGE;
typedef struct {int unused;} VLC_Context;
typedef struct {int unused;} VLC_TableV3;

#define	setRECT(_r_, _x_, _y_, _w_, _h_)	((_r_)->x = (_x_), (_r_)->y = (_y_), (_r_)->w = (_w_), (_r_)->h = (_h_))
#define	DECDCT_MODE_24BPP		1
#define	DECDCT_MODE_16BPP_BIT15	3
#define	SPU_TRANSFER_WAIT		1
#define	PCDRV_READ				0
#define	PCDRV_SEEK_SET			0
#define	PCDRV_SEEK_END			2
#define	MDEC1					host_mdec1

static volatile uint16_t	host_spu[0x200];
static volatile uint32_t	host_irq_mask, host_lines, host_lines_mode, host_mdec1;

static int		VSync(int mode);
static int		DrawSync(int mode);
static void		LoadImage(const RECT *r, const uint32_t *data);
static void		ClearImage(const RECT *r, int red, int green, int blue);
static void		DecDCTReset(int mode);
static void		DecDCTPutEnv(const void *env, int mono);
static int		DecDCTvlcStart(VLC_Context *ctx, uint32_t *buf, size_t max, const uint32_t *bs);
static void		DecDCTin(const uint32_t *data, int mode);
static void		DecDCTout(uint32_t *data, size_t words);
static int		DecDCToutSync(int mode);
static void		SpuSetTransferStartAddr(uint32_t adrs);
static size_t	SpuWrite(const uint32_t *data, size_t size);
static int		SpuIsTransferCompleted(int mode);
static int		PCopen(const char *name, int flags, int perms);
static int		PCclose(int fd);
static int		PCread(int fd, char *buf, int size);
static int		PClseek(int fd, int offset, int mode);

static void		HostCopy(int buf, int offset, const uint8_t *src, int n);
static void		HostDecode(int slot, int w, int h);
static void		HostShown(int slot);

#endif
