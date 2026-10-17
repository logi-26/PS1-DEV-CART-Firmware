/*
* PS1 DEV CART TMD model viewer
* Uploaded and executed by "ps1transfer -m model.tmd [-t texture.tim ...]"
*
* ps1transfer uploads a data block at VIEWER_DATA_ADRS (header + TMD + TIMs) before executing this program.
* The TIM images are loaded in VRAM at their own position (TMD textures refer to these VRAM positions),
* the model is centered using its bounding box.
*
* Controls:	D-pad: rotate X/Y, L1/R1: rotate Z, Triangle/Cross: zoom in/out, Start: reset view,
*			Select: hide/show the information text
*
* Built with PSn00bSDK (open source) instead of PsyQ. PsyQ's libgs (GsSortObject4) is replaced by our own
* TMD renderer with the GTE: all the polygon types (triangles and quads; flat or gouraud; textured or not; lit with
* normals, or unlit with colors per polygon or per vertex), same flat light and ambient as before, back faces removed
* unless the polygon is double sided. Lines and sprites are skipped.
*
* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
* If a copy of the MPL was not distributed with this file,
* You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <psxgpu.h>
#include <psxgte.h>
#include <psxetc.h>
#include <psxapi.h>
#include <inline_c.h>
#include "psyq_compat.h"
#include "devcart_ui.h"

// Data block written by ps1transfer, see "-m" in ps1transfer/main.c
#define	VIEWER_DATA_ADRS	0x80100000
#define	VIEWER_MAGIC		0x56444D54	// "TMDV"
#define	VIEWER_MAX_TIM		8

typedef struct
{
	uint32_t	magic;
	uint32_t	tmd_adrs;
	uint32_t	tim_count;
	uint32_t	tim_adrs[VIEWER_MAX_TIM];
} ViewerData;

#define	SCREEN_W		320
#define	SCREEN_H		240
#define	OT_LEN			4096
#define	OT_SHIFT		2				// Ordering table entry = average Z >> OT_SHIFT
#define	MAX_OBJECTS		32
#define	PACKET_SIZE		(192 * 1024)	// Per buffer, about 4000 textured polygons

#define	ROT_SPEED		32				// 4096 = 360 degrees
#define	ZOOM_SPEED		40				// 1/1024 of the distance per frame

// TMD primitive: olen, ilen (data words), flag, mode, then the data
#define	TMD_FLAG_LGT	1				// No lighting: colors given
#define	TMD_FLAG_FCE	2				// Double sided
#define	TMD_FLAG_GRD	4				// Gradation (a color per vertex) with lighting
#define	TMD_MODE_TGE	0x01			// Texture without brightness
#define	TMD_MODE_ABE	0x02			// Semi-transparency
#define	TMD_MODE_TME	0x04			// Textured
#define	TMD_MODE_QUAD	0x08
#define	TMD_MODE_IIP	0x10			// Gouraud
#define	TMD_MODE_CODE	0xE0			// 0x20: polygon (0x40 line, 0x60 sprite)

typedef struct
{
	SVECTOR		*vertices;
	SVECTOR		*normals;
	uint32_t	*primitives;
	uint32_t	n_primitives;
} TmdObject;

static DISPENV		disp[2];
static DRAWENV		draw[2];
static uint32_t		ot[2][OT_LEN];
static uint8_t		packet[2][PACKET_SIZE];
static uint8_t		*packet_ptr, *packet_end;
static TmdObject	objects[MAX_OBJECTS];

static int			n_objects, n_vertices, n_primitives, n_textures, n_drawn;
static int			fnt_info;		// Text of the info panel (devcart_ui.h)
static int32_t		model_radius;
static VECTOR		model_center;

static void	LoadTextures(ViewerData *vd)
{
	TIM_IMAGE	tim;
	int			i;

	for (i = 0; (i < vd->tim_count) && (i < VIEWER_MAX_TIM); i++)
	{
		uint32_t	*p = (uint32_t *)vd->tim_adrs[i];

		if (*p != 0x10)	// TIM ID
			continue;
		GetTimInfo(p, &tim);
		LoadImage(tim.prect, tim.paddr);
		if (tim.mode & 8)	// CLUT present
			LoadImage(tim.crect, tim.caddr);
		DrawSync(0);
		n_textures++;
	}
}

// TMD: ID, flags, number of objects, then object table (vertex top, n vertex, normal top, n normal, primitive top,
// n primitive, scale). The tops are offsets from the object table, or addresses if flags bit 0 (FIXP) is set.
static int	LoadModel(uint32_t *tmd)
{
	uint32_t	*p = tmd, *table, flags, base;
	int32_t		min[3] = {32767, 32767, 32767}, max[3] = {-32768, -32768, -32768};
	int32_t		d;
	int			i, j, k;

	if (*p++ != 0x41)	// TMD ID
		return (0);
	flags = *p++;
	n_objects = *p++;
	if (n_objects > MAX_OBJECTS)
		n_objects = MAX_OBJECTS;
	table = p;
	base = (flags & 1) ? 0 : (uint32_t)table;

	for (i = 0; i < n_objects; i++)
	{
		uint32_t	*obj = table + i * 7;
		SVECTOR		*v = (SVECTOR *)(base + obj[0]);

		objects[i].vertices = v;
		objects[i].normals = (SVECTOR *)(base + obj[2]);
		objects[i].primitives = (uint32_t *)(base + obj[4]);
		objects[i].n_primitives = obj[5];
		n_vertices += obj[1];
		n_primitives += obj[5];
		for (j = 0; j < obj[1]; j++, v++)
		{
			int16_t	c[3] = {v->vx, v->vy, v->vz};

			for (k = 0; k < 3; k++)
			{
				if (c[k] < min[k])
					min[k] = c[k];
				if (c[k] > max[k])
					max[k] = c[k];
			}
		}
	}

	// The model is rotated around the center of its bounding box
	model_center.vx = (min[0] + max[0]) / 2;
	model_center.vy = (min[1] + max[1]) / 2;
	model_center.vz = (min[2] + max[2]) / 2;

	model_radius = 0;
	for (k = 0; k < 3; k++)
	{
		d = (max[k] - min[k]) / 2;
		if (d > model_radius)
			model_radius = d;
	}
	if (model_radius < 16)
		model_radius = 16;

	// Shown on the computer with "ps1transfer -m model.tmd -c"
	printf("TMD: %d objects, %d vertices, %d polygons\n", n_objects, n_vertices, n_primitives);
	printf("Bounding box: X %d..%d  Y %d..%d  Z %d..%d, radius %d\n", (int)min[0], (int)max[0], (int)min[1],
		(int)max[1], (int)min[2], (int)max[2], (int)model_radius);

	return (n_objects);
}

/****************************************/
// Renderer

// Same light as the PsyQ version (GsSetFlatLight 0: direction 100,100,100, white; ambient 1/3): GTE light matrix row 0 =
// light direction (unit vector, 4096 = 1), color matrix column 0 = light color, back color = ambient
static const MATRIX	light_dir = {{{-2365, -2365, -2365}, {0, 0, 0}, {0, 0, 0}}, {0, 0, 0}};	// Faces against it are lit
static const MATRIX	light_color = {{{4096, 0, 0}, {4096, 0, 0}, {4096, 0, 0}}, {0, 0, 0}};

static void	*AllocPacket(int size)
{
	void	*p = packet_ptr;

	if (packet_ptr + size > packet_end)
		return (NULL);
	packet_ptr += size;
	return (p);
}

// Lighting of a color with a normal (GTE NCCS: color * (ambient + light color * max(0, light . normal)))
static void	Light(SVECTOR *normal, const uint8_t *rgb, uint8_t *out)
{
	CVECTOR	c = {rgb[0], rgb[1], rgb[2], 0}, res;

	gte_ldv0(normal);
	gte_ldrgb(&c);
	gte_nccs();
	gte_strgb(&res);
	out[0] = res.r;
	out[1] = res.g;
	out[2] = res.b;
}

// One TMD polygon: data parsed in the order of the TMD format (texture coordinates, colors, then normal/vertex
// indices), projected with the GTE (rotation, translation, perspective set before), added to the ordering table
static void	DrawPolygon(TmdObject *obj, uint32_t *prim, uint32_t *otp)
{
	uint8_t		flag = (prim[0] >> 16) & 0xFF, mode = prim[0] >> 24;
	uint8_t		*data = (uint8_t *)(prim + 1), *uv = NULL, *colors;
	uint16_t	*idx, vert[4], norm[4];
	int			n = (mode & TMD_MODE_QUAD) ? 4 : 3, textured = mode & TMD_MODE_TME, gouraud = mode & TMD_MODE_IIP;
	int			lit = !(flag & TMD_FLAG_LGT), n_colors, n_normals, shaded, i;
	SVECTOR		*v = obj->vertices;
	int32_t		opz, otz, sz[3];
	uint32_t	xy[4];
	uint8_t		rgb[4][3];
	void		*p;
	static const uint8_t	white[3] = {128, 128, 128};	// Textured: 128 = texture colors unchanged

	// Texture: u, v, CBA (clut) / TSB (texture page) / unused, one word per vertex
	if (textured)
	{
		uv = data;
		data += n * 4;
	}
	// Colors (r, g, b, mode or unused): untextured polygons have at least one, one per vertex with gradation (lit)
	// or gouraud (unlit); textured polygons have none when lit, else one (flat) or one per vertex (gouraud)
	n_colors = (!textured || !lit) ? 1 : 0;
	if (n_colors && ((lit && (flag & TMD_FLAG_GRD)) || (!lit && gouraud)))
		n_colors = n;
	colors = data;
	data += n_colors * 4;
	// Indices (16-bit): lit flat or with gradation: normal then vertices, lit gouraud: normal, vertex for each
	// vertex, unlit: vertices
	idx = (uint16_t *)data;
	n_normals = lit ? ((gouraud && !(flag & TMD_FLAG_GRD)) ? n : 1) : 0;
	for (i = 0; i < n; i++)
	{
		if (lit && ((n_normals > 1) || !i))
			norm[i] = *idx++;
		vert[i] = *idx++;
	}

	// Projection of the first 3 vertices: back face, Z, then the 4th vertex
	gte_ldv3(&v[vert[0]], &v[vert[1]], &v[vert[2]]);
	gte_rtpt();
	gte_nclip();
	gte_stopz(&opz);
	if ((opz <= 0) && !(flag & TMD_FLAG_FCE))
		return;
	gte_stsz3(&sz[0], &sz[1], &sz[2]);
	if ((sz[0] < 8) || (sz[1] < 8) || (sz[2] < 8))	// Behind or too near the camera
		return;
	gte_stsxy3(&xy[0], &xy[1], &xy[2]);
	gte_avsz3();
	gte_stotz(&otz);
	otz >>= OT_SHIFT;
	if ((otz <= 0) || (otz >= OT_LEN))
		return;
	if (n == 4)
	{
		gte_ldv0(&v[vert[3]]);
		gte_rtps();
		gte_stsxy(&xy[3]);
	}

	// Colors of the vertices: lighting with the normals, or the colors of the data
	for (i = 0; i < n; i++)
	{
		const uint8_t	*base = (textured && lit) ? white : colors + ((n_colors > 1) ? i * 4 : 0);

		if (lit)
			Light(&obj->normals[norm[(n_normals > 1) ? i : 0]], base, rgb[i]);
		else
			memcpy(rgb[i], base, 3);
	}
	// A color per vertex (gouraud shading on the GPU) or one for the polygon
	shaded = (n_colors > 1) || (n_normals > 1);

	// GPU primitive (POLY_F3/F4/G3/G4/FT3/FT4/GT3/GT4)
	#define	XY(_p_, _i_)	(*(uint32_t *)&(_p_)->x##_i_ = xy[_i_])
	#define	RGB(_p_, _i_)	setRGB##_i_(_p_, rgb[_i_][0], rgb[_i_][1], rgb[_i_][2])
	#define	TEX(_p_)		((_p_)->clut = uv[2] | (uv[3] << 8), (_p_)->tpage = uv[6] | (uv[7] << 8))
	if (textured && shaded && (n == 4))
	{
		POLY_GT4	*t = AllocPacket(sizeof(POLY_GT4));

		if (!(p = t))
			return;
		setPolyGT4(t);
		XY(t, 0); XY(t, 1); XY(t, 2); XY(t, 3);
		RGB(t, 0); RGB(t, 1); RGB(t, 2); RGB(t, 3);
		setUV4(t, uv[0], uv[1], uv[4], uv[5], uv[8], uv[9], uv[12], uv[13]);
		TEX(t);
	}
	else if (textured && shaded)
	{
		POLY_GT3	*t = AllocPacket(sizeof(POLY_GT3));

		if (!(p = t))
			return;
		setPolyGT3(t);
		XY(t, 0); XY(t, 1); XY(t, 2);
		RGB(t, 0); RGB(t, 1); RGB(t, 2);
		setUV3(t, uv[0], uv[1], uv[4], uv[5], uv[8], uv[9]);
		TEX(t);
	}
	else if (textured && (n == 4))
	{
		POLY_FT4	*t = AllocPacket(sizeof(POLY_FT4));

		if (!(p = t))
			return;
		setPolyFT4(t);
		XY(t, 0); XY(t, 1); XY(t, 2); XY(t, 3);
		RGB(t, 0);
		setUV4(t, uv[0], uv[1], uv[4], uv[5], uv[8], uv[9], uv[12], uv[13]);
		TEX(t);
	}
	else if (textured)
	{
		POLY_FT3	*t = AllocPacket(sizeof(POLY_FT3));

		if (!(p = t))
			return;
		setPolyFT3(t);
		XY(t, 0); XY(t, 1); XY(t, 2);
		RGB(t, 0);
		setUV3(t, uv[0], uv[1], uv[4], uv[5], uv[8], uv[9]);
		TEX(t);
	}
	else if (shaded && (n == 4))
	{
		POLY_G4	*g = AllocPacket(sizeof(POLY_G4));

		if (!(p = g))
			return;
		setPolyG4(g);
		XY(g, 0); XY(g, 1); XY(g, 2); XY(g, 3);
		RGB(g, 0); RGB(g, 1); RGB(g, 2); RGB(g, 3);
	}
	else if (shaded)
	{
		POLY_G3	*g = AllocPacket(sizeof(POLY_G3));

		if (!(p = g))
			return;
		setPolyG3(g);
		XY(g, 0); XY(g, 1); XY(g, 2);
		RGB(g, 0); RGB(g, 1); RGB(g, 2);
	}
	else if (n == 4)
	{
		POLY_F4	*f = AllocPacket(sizeof(POLY_F4));

		if (!(p = f))
			return;
		setPolyF4(f);
		XY(f, 0); XY(f, 1); XY(f, 2); XY(f, 3);
		RGB(f, 0);
	}
	else
	{
		POLY_F3	*f = AllocPacket(sizeof(POLY_F3));

		if (!(p = f))
			return;
		setPolyF3(f);
		XY(f, 0); XY(f, 1); XY(f, 2);
		RGB(f, 0);
	}
	#undef XY
	#undef RGB
	#undef TEX
	if (textured && (mode & TMD_MODE_TGE))
		setShadeTex((POLY_FT3 *)p, 1);
	if (mode & TMD_MODE_ABE)
		setSemiTrans((POLY_F3 *)p, 1);
	addPrim(otp + otz, p);
	n_drawn++;
}

static void	DrawObject(TmdObject *obj, uint32_t *otp)
{
	uint32_t	*prim = obj->primitives;
	uint32_t	i;

	for (i = 0; i < obj->n_primitives; i++)
	{
		uint8_t	ilen = (prim[0] >> 8) & 0xFF, mode = prim[0] >> 24;

		if ((mode & TMD_MODE_CODE) == 0x20)		// Polygons (lines and sprites skipped)
			DrawPolygon(obj, prim, otp);
		prim += 1 + ilen;
	}
}

// Local to screen: rotation around the model center, then the camera at (0, 0, -dist) looking at the origin (as
// PsyQ's GsRVIEW2 with vpz = -dist: no rotation, translation (0, 0, dist)); the light follows the model rotation
static void	SetMatrices(SVECTOR *rot, int32_t dist)
{
	VECTOR	center = {-model_center.vx, -model_center.vy, -model_center.vz}, t;
	MATRIX	ls, llm;

	RotMatrix(rot, &ls);
	ApplyMatrixLV(&ls, &center, &t);
	ls.t[0] = t.vx;
	ls.t[1] = t.vy;
	ls.t[2] = t.vz + dist;
	MulMatrix0((MATRIX *)&light_dir, &ls, &llm);	// (uses the GTE rotation matrix: before it is set)
	gte_SetLightMatrix(&llm);
	gte_SetRotMatrix(&ls);
	gte_SetTransMatrix(&ls);
}

int	main(void)
{
	ViewerData	*vd = (ViewerData *)VIEWER_DATA_ADRS;
	SVECTOR		rot = {0, 0, 0};
	int32_t		dist, dist_default, dist_min, dist_max;
	uint32_t	pad, opad = 0;
	int			i, side = 0, ok = 0, show_info = 1, is_pal;

	// Same video mode as the console region, like the PS1 DEV CART menu (BIOS version string: 'E' = Europe = PAL)
	is_pal = (*((char *)0xBFC7FF52) == 'E');
	ResetGraph(0);	// (first: PSn00bSDK's ResetGraph sets up the callbacks, see psyq_compat.h)
	SetVideoMode(is_pal ? MODE_PAL : MODE_NTSC);
	PadInit(0);
	for (i = 0; i < 2; i++)
	{
		SetDefDispEnv(&disp[i], 0, i ? 0 : SCREEN_H, SCREEN_W, SCREEN_H);
		SetDefDrawEnv(&draw[i], 0, i ? SCREEN_H : 0, SCREEN_W, SCREEN_H);
	}
	FntLoad(960, 256);
	UiInit(draw);	// (background: UI_BG)
	fnt_info = UiOpenText(8, 20, 304, 2);

	// GTE: projection plane at 320 (as GsSetProjection(SCREEN_W)), screen center, average Z of 3/4 vertices
	InitGeom();
	gte_SetGeomOffset(SCREEN_W / 2, SCREEN_H / 2);
	gte_SetGeomScreen(SCREEN_W);
	__asm__ volatile ("ctc2 %0, $29; ctc2 %1, $30" : : "r"(0x155), "r"(0x100));	// ZSF3 = 1/3, ZSF4 = 1/4
	gte_SetColorMatrix((MATRIX *)&light_color);
	gte_SetBackColor(85, 85, 85);	// Ambient 1/3


	if (vd->magic == VIEWER_MAGIC)
	{
		LoadTextures(vd);
		ok = LoadModel((uint32_t *)vd->tmd_adrs);
	}

	// Camera distance from the model size: the whole model is visible at the start
	dist_default = model_radius * 3;
	dist_min = model_radius / 4;
	dist_max = model_radius * 40;
	dist = dist_default;

	SetDispMask(1);
	printf("Video mode %s, camera distance %d (min %d, max %d)\n", is_pal ? "PAL" : "NTSC", (int)dist, (int)dist_min,
		(int)dist_max);

	while (1)
	{
		pad = PadRead(1);

		if (pad & PADLup)		rot.vx -= ROT_SPEED;
		if (pad & PADLdown)		rot.vx += ROT_SPEED;
		if (pad & PADLleft)		rot.vy -= ROT_SPEED;
		if (pad & PADLright)	rot.vy += ROT_SPEED;
		if (pad & PADL1)		rot.vz -= ROT_SPEED;
		if (pad & PADR1)		rot.vz += ROT_SPEED;
		if ((pad & PADRup) && (dist > dist_min))	dist -= (dist * ZOOM_SPEED) >> 10;
		if ((pad & PADRdown) && (dist < dist_max))	dist += ((dist * ZOOM_SPEED) >> 10) + 1;
		if (pad & PADstart)
		{
			rot.vx = rot.vy = rot.vz = 0;
			dist = dist_default;
		}
		if ((pad & PADselect) && !(opad & PADselect))
			show_info = !show_info;
		opad = pad;

		side ^= 1;
		ClearOTagR(ot[side], OT_LEN);
		packet_ptr = packet[side];
		packet_end = packet[side] + PACKET_SIZE;
		n_drawn = 0;
		SetMatrices(&rot, dist);
		for (i = 0; i < n_objects; i++)
			DrawObject(&objects[i], ot[side]);

		// Screen (devcart_ui.h): title bar over the model, model details and controls (SELECT shows/hides them)
		if (!ok)
		{
			UiTitle("PS1 DEV CART TMD VIEWER", "NO MODEL");
			UiTextPanel(8, 20, 304, 2);
			FntPrint(fnt_info, "NO VALID TMD MODEL FOUND. USE:\nPS1TRANSFER -M MODEL.TMD [-T TEX.TIM]");
		}
		else
		{
			char	right[20];

			sprintf(right, "%d POLYGONS", n_primitives);
			UiTitle("PS1 DEV CART TMD VIEWER", right);
			if (show_info)
			{
				UiTextPanel(8, 20, 304, 2);
				FntPrint(fnt_info, "OBJECTS %d  VERTICES %d\nPOLYGONS %d  TIMS %d", n_objects, n_vertices, n_primitives,
					n_textures);
				UiHelp("D-PAD L1/R1:ROTATE  TRI/X:ZOOM\nSTART:RESET  SELECT:HIDE INFO");
			}
		}

		DrawSync(0);
		VSync(0);
		PutDispEnv(&disp[side]);
		PutDrawEnv(&draw[side]);
		DrawOTag(&ot[side][OT_LEN - 1]);
		UiFlush();
		FntFlush(fnt_info);
	}

	return (0);
}
