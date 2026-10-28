/*
* PS1 DEV CART TMD model viewer
* Uploaded and executed by "ps1transfer -m model.tmd [-t texture.tim ...] [-tod anim.tod ...]"
*
* ps1transfer uploads a data block at VIEWER_DATA_ADRS (header + TMD + TIMs + TODs) before executing this program.
* The TIM images are loaded in VRAM at their own position (TMD textures refer to these VRAM positions),
* the model is centered using its bounding box.
*
* TOD animations (Sony's format, as played by PsyQ's TOD sample functions TodSetFrame/TodSetPacket): the frames'
* packets create the objects and link them to the TMD objects (rsdlink's IDs: TMD object n = ID n + 1), set their
* parent (a hierarchy of local coordinates, as GsCOORDINATE2) and attributes, and move them (rotation, scale,
* translation, absolute or from the previous frame). Each object is drawn with its own matrix. The animation plays at
* the file's time resolution (1/60 s units), loops, can be paused, stepped, played slower or faster. A TOD that
* doesn't create its objects (it continues another animation) starts from the end of the first TOD that does (as
* PsyQ's sample plays APPEAR.TOD before the others). Light, camera and matrix packets are not used (counted).
*
* Controls:	D-pad: rotate X/Y, L1/R1: rotate Z, Triangle/Cross: zoom in/out, Start: reset view,
*			Select: hide/show the information text,
*			with TODs: Circle: play/pause, L2/R2: previous/next frame (held: repeat), Square: next TOD
* PS1 Dev Cart Manager ("ps1transfer -m ... -mdllive", see ps1transfer/mdllive.c): the TOD, frame, playback and view
* are shared with the computer (ModelSync, written at each frame), which can change them (requests applied at the next
* frame): its controls and the controller's act on the same viewer.
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
#define	VIEWER_MAX_TOD		16

typedef struct
{
	uint32_t	adrs;
	uint32_t	size;
	char		name[24];
} TodEntry;

typedef struct
{
	uint32_t	magic;
	uint32_t	tmd_adrs;
	uint32_t	tim_count;
	uint32_t	tim_adrs[VIEWER_MAX_TIM];
	uint32_t	reserved[5];
	uint32_t	tod_count;					// (0x40)
	TodEntry	tods[VIEWER_MAX_TOD];
} ViewerData;

// State shared with the PS1 Dev Cart Manager ("ps1transfer -mdllive", see ps1transfer/mdllive.c), in the data block
// header: written at each frame, and the changes the computer asks for (applied at the next frame, then set back)
#define	VIEWER_SYNC_ADRS	(VIEWER_DATA_ADRS + 0x300)
#define	VIEWER_SYNC_MAGIC	0x534C444D	// "MDLS"
#define	REQ_NONE			-32768

typedef struct
{
	uint32_t	magic;
	int16_t		tod_count;
	int16_t		tod;				// TOD played (-1: none)
	int16_t		frame;				// Its frame (frame number of the file)
	int16_t		first, last;		// Its first and last frame numbers
	int16_t		playing, loop;
	int16_t		speed;				// Percent
	int16_t		rot_x, rot_y, rot_z;	// View (4096 = 360 degrees)
	int16_t		objects;			// Objects drawn
	int32_t		dist, dist_min, dist_max;	// Camera distance and its limits
	int32_t		polygons;			// Polygons drawn
	int32_t		skipped;			// Packets not used (light, camera, matrix...)
	int16_t		req_tod, req_frame, req_play, req_loop, req_speed, req_view;	// REQ_NONE: nothing; view 1: reset
	int16_t		req_rot;			// 1: the view rotation below
	int16_t		req_rot_x, req_rot_y, req_rot_z;
	int32_t		req_dist;			// -1: nothing
} ModelSync;
// (offsets used by ps1transfer/mdllive.c)
_Static_assert(__builtin_offsetof(ModelSync, dist) == 28, "ModelSync: see mdllive.c");
_Static_assert(__builtin_offsetof(ModelSync, req_tod) == 48, "ModelSync: see mdllive.c");
_Static_assert(__builtin_offsetof(ModelSync, req_rot) == 60, "ModelSync: see mdllive.c");
_Static_assert(__builtin_offsetof(ModelSync, req_dist) == 68 && sizeof(ModelSync) == 72, "ModelSync: see mdllive.c");
_Static_assert(__builtin_offsetof(ViewerData, tod_count) == 0x40, "ViewerData: see UploadModelData in main.c");

#define	SCREEN_W		320
#define	SCREEN_H		240
#define	OT_LEN			4096
#define	OT_SHIFT		2				// Ordering table entry = average Z >> OT_SHIFT
#define	MAX_OBJECTS		32
#define	PACKET_SIZE		(192 * 1024)	// Per buffer, about 4000 textured polygons

#define	ROT_SPEED		32				// 4096 = 360 degrees
#define	ZOOM_SPEED		40				// 1/1024 of the distance per frame
#define	MAX_TOD_OBJ		64				// TOD objects (parts and joints)
#define	MAX_TOD_FRAMES	8192			// Frames of all the TODs
#define	STEP_DELAY		20				// L2/R2 held: frames before the repeat, then a frame every STEP_REPEAT
#define	STEP_REPEAT		3

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
	uint32_t	n_vertices;
} TmdObject;

static DISPENV		disp[2];
static DRAWENV		draw[2];
static uint32_t		ot[2][OT_LEN];
static uint8_t		packet[2][PACKET_SIZE];
static uint8_t		*packet_ptr, *packet_end;
static TmdObject	objects[MAX_OBJECTS];

static int			n_objects, n_vertices, n_primitives, n_textures, n_drawn;
static int			fnt_info;		// Text of the info panel (devcart_ui.h)
static int32_t		model_radius, model_radius_all;	// The model's size, and with the animation's moves
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
		objects[i].n_vertices = obj[1];
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
// TOD animation (Sony's format: header ID 0x50, version, time resolution; number of frames; then each frame: size in
// words and number of packets, frame number, packets: object ID, type, flags, length in words, data)

enum	{TOD_ATTR, TOD_COORD, TOD_TMDID, TOD_PARENT, TOD_MATRIX, TOD_TMDDATA, TOD_LIGHT, TOD_CAMERA, TOD_OBJCTL};

typedef struct
{
	uint32_t	id;				// TOD object ID, TOD_FREE: free slot
	int16_t		tmd;			// TMD object drawn, -1: none (a joint)
	int16_t		parent;			// Index of the parent, -1: world
	uint32_t	attr;			// Bit 31: not displayed (GsDOFF)
	SVECTOR		rot;			// Coordinate parameters (GsCOORD2PARAM)
	VECTOR		scale, trans;
	MATRIX		coord;			// Local matrix (in the parent's coordinates)
} TodObj;

typedef struct
{
	TodObj	obj[MAX_TOD_OBJ];
	int		n;
} TodTable;

typedef struct
{
	uint32_t	**frames;		// Frame pointers (in the frame pool)
	int			n_frames;
	int			res;			// 1/60 s per frame number
	int			first, last;	// Frame numbers
	int			creates;		// Creates its objects (else continues the first TOD that does)
	char		name[24];
} Tod;

#define	TOD_FREE		0xFFFFFFFF

static Tod			tods[VIEWER_MAX_TOD];
static int			n_tods;
static uint32_t		*frame_pool[MAX_TOD_FRAMES];
static int			frame_pool_used;
static TodTable		table, setup;		// Objects now, and at the end of the first TOD that creates them
static int			have_setup;
static int			cur_tod = -1, next_frame, cur_frame, playing = 1, looping = 1, speed = 100, skipped;
static uint32_t		tod_clock;			// Time in 1/60 s, 8 bits of fraction
static MATRIX		world[MAX_TOD_OBJ];
static uint8_t		world_state[MAX_TOD_OBJ];	// 0: to compute, 1: computing, 2: done

static void	LoadTods(ViewerData *vd)
{
	int	i, f, k;

	for (i = 0; (i < vd->tod_count) && (i < VIEWER_MAX_TOD); i++)
	{
		uint32_t	*p = (uint32_t *)vd->tods[i].adrs, *end = p + vd->tods[i].size / 4, *fp;
		Tod			*t = &tods[n_tods];
		int			n = p[1];

		if (((p[0] & 0xFF) != 0x50) || (frame_pool_used + n > MAX_TOD_FRAMES))
			continue;
		memcpy(t->name, vd->tods[i].name, sizeof(t->name));
		t->name[sizeof(t->name) - 1] = 0;
		t->res = (p[0] >> 16) ? (p[0] >> 16) : 1;
		t->frames = &frame_pool[frame_pool_used];
		t->creates = 0;
		for (f = 0, fp = p + 2; (f < n) && (fp + 2 <= end) && (fp[0] & 0xFFFF); f++, fp += fp[0] & 0xFFFF)
		{
			uint32_t	*pk = fp + 2;

			t->frames[f] = fp;
			for (k = 0; (k < (fp[0] >> 16)) && (pk < end); k++, pk += (pk[0] >> 24) ? (pk[0] >> 24) : 1)
				if ((((pk[0] >> 16) & 15) == TOD_OBJCTL) && !((pk[0] >> 20) & 15))
					t->creates = 1;
		}
		if (!f)
			continue;
		t->n_frames = f;
		t->first = t->frames[0][1];
		t->last = t->frames[f - 1][1];
		frame_pool_used += f;
		n_tods++;
		printf("TOD %s: %d frames (%d..%d), %d/60 s per frame%s\n", t->name, f, t->first, t->last, t->res,
			t->creates ? ", creates its objects" : "");
	}
}

static int	FindObj(uint32_t id)
{
	int	i;

	for (i = 0; i < table.n; i++)
		if (table.obj[i].id == id)
			return (i);
	return (-1);
}

static void	InitObj(TodObj *o, uint32_t id)
{
	static const MATRIX	identity = {{{ONE, 0, 0}, {0, ONE, 0}, {0, 0, ONE}}, {0, 0, 0}};

	o->id = id;
	o->tmd = -1;
	o->parent = -1;
	o->attr = 0;
	o->rot.vx = o->rot.vy = o->rot.vz = 0;
	o->scale.vx = o->scale.vy = o->scale.vz = ONE;
	o->trans.vx = o->trans.vy = o->trans.vz = 0;
	o->coord = identity;
}

static void	CreateObj(uint32_t id)
{
	int	i;

	if (FindObj(id) >= 0)
		return;
	for (i = 0; (i < table.n) && (table.obj[i].id != TOD_FREE); i++)	// A free slot, else at the end
		;
	if (i >= MAX_TOD_OBJ)
		return;
	if (i == table.n)
		table.n++;
	InitObj(&table.obj[i], id);
}

static void	RemoveObj(uint32_t id)
{
	int	i = FindObj(id), j;

	if (i < 0)
		return;
	table.obj[i].id = TOD_FREE;
	for (j = 0; j < table.n; j++)			// (its children are now in the world)
		if (table.obj[j].parent == i)
			table.obj[j].parent = -1;
	while (table.n && (table.obj[table.n - 1].id == TOD_FREE))
		table.n--;
}

// One packet (as TodSetPacket): returns the next one
static uint32_t	*TodPacket(uint32_t *pk)
{
	uint32_t	hdr = pk[0], *d = pk + 1;
	int			type = (hdr >> 16) & 15, flag = (hdr >> 20) & 15, len = hdr >> 24, i;
	int32_t		*l = (int32_t *)d;
	int16_t		*h = (int16_t *)d;
	TodObj		dummy, *o;

	i = FindObj(hdr & 0xFFFF);
	o = (i >= 0) ? &table.obj[i] : &dummy;
	if (i < 0)
		InitObj(&dummy, TOD_FREE);
	switch (type)
	{
		case TOD_ATTR:
			o->attr = (o->attr & d[0]) | d[1];
			break;
		case TOD_COORD:
			if (flag & 1)		// From the previous frame: rotation, scale, translation added (scale multiplied)
			{
				if (flag & 2)
				{
					o->rot.vx += l[0] / 360;
					o->rot.vy += l[1] / 360;
					o->rot.vz += l[2] / 360;
					l += 3;
				}
				if (flag & 4)
				{
					h = (int16_t *)l;
					o->scale.vx = o->scale.vx * h[0] / 4096;
					o->scale.vy = o->scale.vy * h[1] / 4096;
					o->scale.vz = o->scale.vz * h[2] / 4096;
					l += 2;
				}
				if (flag & 8)
				{
					o->trans.vx += l[0];
					o->trans.vy += l[1];
					o->trans.vz += l[2];
				}
				RotMatrix(&o->rot, &o->coord);
				ScaleMatrix(&o->coord, &o->scale);
				TransMatrix(&o->coord, &o->trans);
			}
			else				// Absolute values (only the ones given change)
			{
				if (flag & 2)
				{
					o->rot.vx = l[0] / 360;
					o->rot.vy = l[1] / 360;
					o->rot.vz = l[2] / 360;
					l += 3;
					RotMatrix(&o->rot, &o->coord);
				}
				if (flag & 4)
				{
					h = (int16_t *)l;
					o->scale.vx = h[0];
					o->scale.vy = h[1];
					o->scale.vz = h[2];
					l += 2;
					if (!(flag & 2))
						RotMatrix(&o->rot, &o->coord);
					ScaleMatrix(&o->coord, &o->scale);
				}
				if (flag & 8)
				{
					o->trans.vx = l[0];
					o->trans.vy = l[1];
					o->trans.vz = l[2];
					TransMatrix(&o->coord, &o->trans);
				}
			}
			break;
		case TOD_TMDID:		// rsdlink's TMD IDs: object n has ID n + 1
			o->tmd = (((d[0] & 0xFFFF) >= 1) && ((d[0] & 0xFFFF) <= n_objects)) ? (d[0] & 0xFFFF) - 1 : -1;
			break;
		case TOD_PARENT:
			if ((i >= 0) && (!d[0] || (d[0] == 0xFFFF)))
				o->parent = -1;
			else if (i >= 0)
			{
				int	parent = FindObj(d[0]);

				o->parent = (parent != i) ? parent : -1;
			}
			break;
		case TOD_OBJCTL:
			if (flag == 0)
				CreateObj(hdr & 0xFFFF);
			else if (flag == 1)
				RemoveObj(hdr & 0xFFFF);
			break;
		default:			// Matrix, TMD data, light, camera, user packets
			skipped++;
			break;
	}
	return (pk + (len ? len : 1));
}

static void	TodFrame(Tod *t, int index)
{
	uint32_t	*fp = t->frames[index], *pk = fp + 2;
	int			i;

	for (i = 0; i < (fp[0] >> 16); i++)
		pk = TodPacket(pk);
}

// The TOD from its start: the objects as it expects them, its first frame
static void	TodStart(int index)
{
	Tod	*t = &tods[index];

	cur_tod = index;
	if (t->creates || !have_setup)
		table.n = 0;
	else
		table = setup;
	next_frame = 0;
	while ((next_frame < t->n_frames) && (t->frames[next_frame][1] <= t->first))
		TodFrame(t, next_frame++);
	cur_frame = t->first;
	tod_clock = (uint32_t)t->first * t->res << 8;
}

// The TOD's frames up to frame f (back: from its start again)
static void	TodGoTo(int f)
{
	Tod	*t = &tods[cur_tod];

	f = (f < t->first) ? t->first : ((f > t->last) ? t->last : f);
	if (f < cur_frame)
		TodStart(cur_tod);
	while ((next_frame < t->n_frames) && (t->frames[next_frame][1] <= f))
		TodFrame(t, next_frame++);
	cur_frame = f;
}

// Frame f shown (step, seek): the playback time starts from it
static void	TodSeek(int f)
{
	TodGoTo(f);
	tod_clock = (uint32_t)cur_frame * tods[cur_tod].res << 8;
}

// Playback: the TOD's time goes on by the video frames elapsed (PAL: 1.2/60 s each; a big model takes several to
// draw, its animation keeps its speed), at the speed chosen
static void	TodTick(int is_pal, int vsyncs)
{
	Tod			*t = &tods[cur_tod];
	uint32_t	target;

	if (!playing)
		return;
	tod_clock += (uint32_t)speed * 256 * (is_pal ? 6 : 5) * vsyncs / 500;
	target = (tod_clock >> 8) / t->res;
	if (target > t->last)
	{
		if (looping)
			TodStart(cur_tod);
		else
		{
			TodSeek(t->last);
			playing = 0;
		}
	}
	else if (target > cur_frame)
		TodGoTo(target);		// (the time keeps its fraction of a frame)
}

// World matrix of an object: its parents' matrices, then its own (as GsGetLw)
static MATRIX	*World(int i)
{
	TodObj	*o = &table.obj[i];

	if (world_state[i] == 2)
		return (&world[i]);
	if ((o->parent >= 0) && (o->parent < table.n) && (world_state[o->parent] != 1))	// (1: a loop, ignored)
	{
		MATRIX	*pw;

		world_state[i] = 1;
		pw = World(o->parent);
		CompMatrixLV(pw, &o->coord, &world[i]);
	}
	else
		world[i] = o->coord;
	world_state[i] = 2;
	return (&world[i]);
}

static int	Drawn(TodObj *o)
{
	return ((o->id != TOD_FREE) && (o->tmd >= 0) && !(o->attr & 0x80000000));
}

// The camera framing: the bounding box of the vertices drawn in the world (each object moved by its world matrix with
// the GTE, as drawn), over the frames of a TOD
static int32_t	box_min[3], box_max[3];
static int		box_n;

static void	BoxAdd(void)
{
	int32_t	*min = box_min, *max = box_max;
	int		i, j, k;

	memset(world_state, 0, sizeof(world_state));
	for (i = 0; i < table.n; i++)
	{
		TmdObject	*obj;
		MATRIX		*w;

		if (!Drawn(&table.obj[i]))
			continue;
		obj = &objects[table.obj[i].tmd];
		w = World(i);
		gte_SetRotMatrix(w);
		gte_SetTransMatrix(w);
		for (j = 0; j < obj->n_vertices; j++)
		{
			int32_t	v[3];

			gte_ldv0(&obj->vertices[j]);
			gte_rtv0tr();
			gte_stlvnl(v);
			for (k = 0; k < 3; k++)
			{
				if (v[k] < min[k])
					min[k] = v[k];
				if (v[k] > max[k])
					max[k] = v[k];
			}
		}
		box_n++;
	}
}

// A TOD chosen: the camera on its whole motion (the objects every 4 frames), then played from its start
static void	TodSelect(int index)
{
	Tod		*t = &tods[index];
	int32_t	d;
	int		f, k;

	for (k = 0; k < 3; k++)
	{
		box_min[k] = 0x7FFFFFFF;
		box_max[k] = -0x7FFFFFFF;
	}
	box_n = 0;
	TodStart(index);
	BoxAdd();
	for (f = 4; f < t->n_frames; f += 4)
	{
		TodGoTo(t->frames[f][1]);
		BoxAdd();
	}
	TodGoTo(t->last);
	BoxAdd();
	if (box_n)
	{
		model_center.vx = (box_min[0] + box_max[0]) / 2;
		model_center.vy = (box_min[1] + box_max[1]) / 2;
		model_center.vz = (box_min[2] + box_max[2]) / 2;
		model_radius_all = 16;
		for (k = 0; k < 3; k++)
		{
			d = (box_max[k] - box_min[k]) / 2;
			if (d > model_radius_all)
				model_radius_all = d;
		}
	}
	TodStart(index);
}

// The TODs ready: the objects at the end of the first one that creates them (for the others), the first TOD playing
static void	TodInit(void)
{
	int	i;

	for (i = 0; i < n_tods; i++)
		if (tods[i].creates)
		{
			TodStart(i);
			TodSeek(tods[i].last);
			setup = table;
			have_setup = 1;
			break;
		}
	TodSelect(0);
	skipped = 0;
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

// TOD: each object drawn with its world matrix, seen from the camera (as SetMatrices: rotation around the center)
static void	DrawTod(SVECTOR *rot, int32_t dist, uint32_t *otp)
{
	VECTOR	center = {-model_center.vx, -model_center.vy, -model_center.vz}, t;
	MATRIX	view, ls, llm;
	int		i;

	RotMatrix(rot, &view);
	ApplyMatrixLV(&view, &center, &t);
	view.t[0] = t.vx;
	view.t[1] = t.vy;
	view.t[2] = t.vz + dist;
	memset(world_state, 0, sizeof(world_state));
	for (i = 0; i < table.n; i++)
	{
		if (!Drawn(&table.obj[i]))
			continue;
		CompMatrixLV(&view, World(i), &ls);			// (uses the GTE matrices: before they are set)
		MulMatrix0((MATRIX *)&light_dir, &ls, &llm);
		gte_SetLightMatrix(&llm);
		gte_SetRotMatrix(&ls);
		gte_SetTransMatrix(&ls);
		DrawObject(&objects[table.obj[i].tmd], otp);
	}
}

int	main(void)
{
	ViewerData	*vd = (ViewerData *)VIEWER_DATA_ADRS;
	ModelSync	*live = (ModelSync *)VIEWER_SYNC_ADRS;
	SVECTOR		rot = {0, 0, 0};
	int32_t		dist, dist_default, dist_min, dist_max;
	uint32_t	pad, opad = 0;
	int			i, side = 0, ok = 0, show_info = 1, is_pal, held = 0, drawn, vsync_last, vsyncs;

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
	fnt_info = UiOpenText(8, 20, 304, 3);

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
		if (ok)
			LoadTods(vd);
	}
	model_radius_all = model_radius;
	if (n_tods)
		TodInit();

	// Camera distance from the model size (with a TOD: its whole motion): the whole model is visible at the start
	dist_default = model_radius_all * 3;
	dist_min = model_radius / 4;
	dist_max = ((model_radius_all > model_radius) ? model_radius_all : model_radius) * 40;
	dist = dist_default;

	// Shared state: the requests cleared, then the magic (the computer waits for it)
	memset(live, 0, sizeof(ModelSync));
	live->req_tod = live->req_frame = live->req_play = live->req_loop = live->req_speed = live->req_view = REQ_NONE;
	live->req_dist = -1;
	vsync_last = VSync(-1);

	SetDispMask(1);
	printf("Video mode %s, camera distance %d (min %d, max %d)\n", is_pal ? "PAL" : "NTSC", (int)dist, (int)dist_min,
		(int)dist_max);

	while (1)
	{
		pad = PadRead(1);

		// Changes asked by the computer
		if ((live->req_tod >= 0) && (live->req_tod < n_tods))
		{
			TodSelect(live->req_tod);
			dist = dist_default = model_radius_all * 3;
			playing = 1;
		}
		if ((live->req_frame != REQ_NONE) && n_tods)
		{
			TodSeek(live->req_frame);
			playing = 0;
		}
		if (live->req_play != REQ_NONE)
			playing = live->req_play ? 1 : 0;
		if (live->req_loop != REQ_NONE)
			looping = live->req_loop ? 1 : 0;
		if ((live->req_speed > 0) && (live->req_speed <= 800))
			speed = live->req_speed;
		if (live->req_rot == 1)
		{
			rot.vx = live->req_rot_x;
			rot.vy = live->req_rot_y;
			rot.vz = live->req_rot_z;
		}
		if (live->req_dist >= 0)
			dist = (live->req_dist < dist_min) ? dist_min : ((live->req_dist > dist_max) ? dist_max : live->req_dist);
		if (live->req_view == 1)
		{
			rot.vx = rot.vy = rot.vz = 0;
			dist = dist_default;
		}
		live->req_tod = live->req_frame = live->req_play = live->req_loop = live->req_speed = live->req_view = REQ_NONE;
		live->req_rot = 0;
		live->req_dist = -1;

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
		if (n_tods)
		{
			if ((pad & PADRright) && !(opad & PADRright))
				playing = !playing;
			if ((pad & PADRleft) && !(opad & PADRleft))
			{
				TodSelect((cur_tod + 1) % n_tods);
				dist = dist_default = model_radius_all * 3;
				playing = 1;
			}
			// L2/R2: a frame back/forward (paused), held: repeated
			if (pad & (PADL2 | PADR2))
			{
				if (!held || ((held >= STEP_DELAY) && !((held - STEP_DELAY) % STEP_REPEAT)))
				{
					playing = 0;
					TodSeek(cur_frame + ((pad & PADR2) ? 1 : -1));
				}
				held++;
			}
			else
				held = 0;
			vsyncs = VSync(-1) - vsync_last;
			TodTick(is_pal, (vsyncs < 1) ? 1 : ((vsyncs > 10) ? 10 : vsyncs));
		}
		vsync_last = VSync(-1);
		opad = pad;
		rot.vx &= 4095;
		rot.vy &= 4095;
		rot.vz &= 4095;

		side ^= 1;
		ClearOTagR(ot[side], OT_LEN);
		packet_ptr = packet[side];
		packet_end = packet[side] + PACKET_SIZE;
		n_drawn = 0;
		if (n_tods)
			DrawTod(&rot, dist, ot[side]);
		else
		{
			SetMatrices(&rot, dist);
			for (i = 0; i < n_objects; i++)
				DrawObject(&objects[i], ot[side]);
		}
		for (i = drawn = 0; i < table.n; i++)
			drawn += n_tods && Drawn(&table.obj[i]);

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
			if (show_info && n_tods)
			{
				Tod	*t = &tods[cur_tod];

				UiTextPanel(8, 20, 304, 3);
				FntPrint(fnt_info, "TOD %d/%d %s\nFRAME %d/%d  %s%s  %d%%\nOBJECTS %d  POLYGONS %d", cur_tod + 1, n_tods,
					t->name, cur_frame, t->last, playing ? "PLAYING" : "PAUSED", looping ? " LOOP" : "", speed, drawn,
					n_drawn);
				UiHelp("DPAD/L1/R1:ROT TRI/X:ZOOM START:VIEW\nO:PLAY L2/R2:FRAME SQUARE:NEXT TOD");
			}
			else if (show_info)
			{
				UiTextPanel(8, 20, 304, 2);
				FntPrint(fnt_info, "OBJECTS %d  VERTICES %d\nPOLYGONS %d  TIMS %d", n_objects, n_vertices, n_primitives,
					n_textures);
				UiHelp("D-PAD L1/R1:ROTATE  TRI/X:ZOOM\nSTART:RESET  SELECT:HIDE INFO");
			}
		}

		// The state shown here, for the computer
		live->tod_count = n_tods;
		live->tod = n_tods ? cur_tod : -1;
		live->frame = n_tods ? cur_frame : 0;
		live->first = n_tods ? tods[cur_tod].first : 0;
		live->last = n_tods ? tods[cur_tod].last : 0;
		live->playing = playing;
		live->loop = looping;
		live->speed = speed;
		live->rot_x = rot.vx;
		live->rot_y = rot.vy;
		live->rot_z = rot.vz;
		live->objects = n_tods ? drawn : n_objects;
		live->dist = dist;
		live->dist_min = dist_min;
		live->dist_max = dist_max;
		live->polygons = n_drawn;
		live->skipped = skipped;
		live->magic = VIEWER_SYNC_MAGIC;	// (once the state is written: the computer waits for it)

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
