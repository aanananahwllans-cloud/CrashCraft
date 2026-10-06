/*
 * CrashCraft: the game half. Everything that touches c1's own state lives here (plain C, c1's
 * headers); host.cpp only sees crashcraft.h.
 */
/* c1 has its own math.h (src/math.h), which shadows the C library's */
extern float tanf(float);
extern float fabsf(float);
extern long lroundf(float);
#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "common.h"
#include "globals.h"
#include "ns.h"
#include "gool.h"
#include "level.h"
#include "gfx.h"
#include "slst.h"
#include "formats/zdat.h"
#include "pc/gfx/soft.h"
#include "pc/gfx/gl.h"
#include "crashcraft.h"

extern ns_struct ns;
extern lid_t cur_lid, next_lid;
extern entry *cur_zone;
extern gool_object *crash;
extern gool_handle handles[8];
extern uint32_t frames_elapsed;
extern int paused;
extern int pad_lock;
extern level_state savestate;
extern vec cam_trans;
extern ang cam_rot;
extern mat16 ms_cam_rot;
extern uint32_t screen_proj;
extern sw_transform_struct params;
extern gl_context context;
extern rect2 screen;
extern SDL_Window *window;
extern int GoolObjectHandleTraverseTreePreorder(gool_object*, int (*)(gool_object*,int), int);

crashcraft_puppet g_crashcraft_puppet;
crashcraft_camera g_crashcraft_camera;

#define ZDAT_TYPE 7
#define BOX_TYPE  0x22
#define DEG_TO_ANG(d) ((int32_t)lroundf((d) * 4096.0f / 360.0f) & 0xFFF)

/* ---- level state --------------------------------------------------------------------------------- */

static int takeover_frames = 0;   /* Crash acts on his own for at least this many more frames */
static int crash_code_frames = 0; /* Crash's own code runs this many frames, hidden, held where Steve is */
static void CrashTakesOver(const char *why, int frames);
static int damage_pending = 0;
static uint32_t last_hurt_frame = 0;

int crashcraft_game_in_level(void) {
  if (!crash || !ns.ldat || !cur_zone) { return 0; }
  if (cur_lid == LID_TITLE || cur_lid == LID_LEVELEND || cur_lid == LID_INTRO || cur_lid == LID_GAMEWIN)
    return 0;
  return next_lid == -1 && ns.draw_skip_counter == 0;
}

int crashcraft_game_area_id(void) {
  return crashcraft_game_in_level() ? (int)cur_lid + 1 : 0;
}

int crashcraft_game_input_owned(void) {
  return !crashcraft_game_in_level() || paused || pause_obj != 0;
}

int crashcraft_game_takeover(void) {
  if (!crashcraft_game_in_level()) { return 1; }
  if (takeover_frames > 0) { return 1; }
  if (crash->status_a & GOOL_FLAG_DYING) { return 1; }
  if (pad_lock) { return 1; }             /* scripted sequences (hog intro, demo playback) */
  if (fade_counter != 0) { return 1; }     /* fading in or out: death, warp, level end */
  return 0;
}

void crashcraft_game_crash_pos(float out[3], float *yaw_deg) {
  if (!crash) {
    out[0] = out[1] = out[2] = 0;
    if (yaw_deg) { *yaw_deg = 0; }
    return;
  }
  out[0] = crash->trans.x / 256.0f;
  out[1] = crash->trans.y / 256.0f;
  out[2] = crash->trans.z / 256.0f;
  /* Crash yaw (12-bit, rot.x): forward = (-sin, 0, -cos); Minecraft yaw = 180 - Crash yaw */
  if (yaw_deg) { *yaw_deg = 180.0f - (crash->rot.x & 0xFFF) * 360.0f / 4096.0f; }
}

/* ---- collision: the level's zone octrees ---------------------------------------------------------- */

typedef struct {
  crashcraft_tri *out;
  int max, count;
  int boxes;
} tri_sink;

static void EmitTri(tri_sink *sink, const float a[3], const float b[3], const float c[3], uint32_t flags) {
  crashcraft_tri *t;
  if (sink->out && sink->count < sink->max) {
    t = &sink->out[sink->count];
    memcpy(&t->v[0], a, sizeof(float)*3);
    memcpy(&t->v[3], b, sizeof(float)*3);
    memcpy(&t->v[6], c, sizeof(float)*3);
    t->flags = flags;
  }
  sink->count++;
}

/* an axis-aligned box as 12 outward-wound triangles; faces in `skip` (bit per face) are left out */
static void EmitBox(tri_sink *sink, const float lo[3], const float hi[3], uint32_t flags) {
  float p[8][3];
  static const int faces[6][4] = {
    { 0, 2, 6, 4 }, /* -x */ { 1, 5, 7, 3 }, /* +x */
    { 0, 4, 5, 1 }, /* -y */ { 2, 3, 7, 6 }, /* +y */
    { 0, 1, 3, 2 }, /* -z */ { 4, 6, 7, 5 }, /* +z */
  };
  int i;

  for (i=0;i<8;i++) {
    p[i][0] = (i & 1) ? hi[0] : lo[0];
    p[i][1] = (i & 2) ? hi[1] : lo[1];
    p[i][2] = (i & 4) ? hi[2] : lo[2];
  }
  for (i=0;i<6;i++) {
    EmitTri(sink, p[faces[i][0]], p[faces[i][1]], p[faces[i][2]], flags);
    EmitTri(sink, p[faces[i][0]], p[faces[i][2]], p[faces[i][3]], flags);
  }
  sink->boxes++;
}

/* Octree leaf -> solid? (see PlotQueryWalls/FindFloorY in solid.c) */
static int LeafSolid(uint16_t node) {
  int type, subtype;
  type = (node & 0xE) >> 1;
  subtype = (node & 0x3F0) >> 4;
  if (type == 3 || type == 4) { return 0; }                 /* pits / triggers */
  if (subtype == 0 || subtype > 38) { return 1; }
  if (subtype >= 7 && subtype <= 10) { return 1; }           /* surface modifiers */
  return 0;                                                  /* event volumes */
}

static uint32_t LeafHazard(uint16_t node) {
  int type, subtype;
  type = (node & 0xE) >> 1;
  subtype = (node & 0x3F0) >> 4;
  switch (subtype) {
  case 3: return GOOL_EVENT_DROWN;
  case 4: return GOOL_EVENT_BURN;
  case 5: return GOOL_EVENT_EXPLODE;
  case 11: return GOOL_EVENT_FALL_KILL;
  case 12: return GOOL_EVENT_SHOCK;
  }
  if (type == 3) {
    if (cur_lid == LID_CORTEXPOWER || cur_lid == LID_TOXICWASTE)
      return GOOL_EVENT_DROWN;
    return GOOL_EVENT_FALL_KILL;
  }
  return 0;
}

typedef void (*leaf_fn)(void *ctx, uint16_t node, const int32_t lo[3], const int32_t hi[3]);

/* every leaf of a zone's octree, with its bounds in object units */
static void OctreeWalk(zone_rect *zr, uint16_t node, const int32_t lo[3], const int32_t dim[3],
  int level, leaf_fn fn, void *ctx) {
  uint16_t *children;
  int32_t sub[3], clo[3], chi[3];
  int flag[3], i, j, k, idx;

  if (!node) { return; }
  if (node & 1) {
    chi[0] = lo[0] + dim[0];
    chi[1] = lo[1] + dim[1];
    chi[2] = lo[2] + dim[2];
    fn(ctx, node, lo, chi);
    return;
  }
  children = (uint16_t*)((uint8_t*)zr + node);
  flag[0] = level < zr->octree.max_depth_x;
  flag[1] = level < zr->octree.max_depth_y;
  flag[2] = level < zr->octree.max_depth_z;
  for (i=0;i<3;i++)
    sub[i] = dim[i] >> flag[i];
  idx = 0;
  for (i=0;i<1+flag[0];i++) {
    for (j=0;j<1+flag[1];j++) {
      for (k=0;k<1+flag[2];k++) {
        clo[0] = lo[0] + (i ? sub[0] : 0);
        clo[1] = lo[1] + (j ? sub[1] : 0);
        clo[2] = lo[2] + (k ? sub[2] : 0);
        OctreeWalk(zr, children[idx], clo, sub, level+1, fn, ctx);
        idx++;
      }
    }
  }
}

static void ZoneWalk(entry *zone, leaf_fn fn, void *ctx) {
  zone_rect *zr;
  int32_t lo[3], dim[3];

  zr = (zone_rect*)zone->items[1];
  lo[0] = zr->x << 8; lo[1] = zr->y << 8; lo[2] = zr->z << 8;
  dim[0] = zr->w << 8; dim[1] = zr->h << 8; dim[2] = zr->d << 8;
  OctreeWalk(zr, zr->octree.root, lo, dim, 0, fn, ctx);
}

/* calls fn(zone) for each zone entry of the level (all pages are resident on pc) */
static void ForEachZone(void (*fn)(entry*, void*), void *ctx) {
  nsd_pte *pte;
  entry *en;
  eid_t eid;
  size_t i;

  if (!ns.nsd) { return; }
  for (i=0;i<ns.nsd->page_table_size;i++) {
    pte = &ns.page_table[i];
    if (pte->eid == EID_NONE) { continue; }
    /* only zones: resolving texture/audio entries this way breaks c1's paging */
    if (NSEIDToString(pte->eid)[4] != 'Z') { continue; }
    eid = pte->eid; /* NSLookup writes the resolved reference back into its argument */
    en = NSLookup(&eid);
    if (ISERRORCODE(en) || !en) { continue; }
    if (en->type == ZDAT_TYPE)
      fn(en, ctx);
  }
}

static void SolidLeaf(void *ctx, uint16_t node, const int32_t lo[3], const int32_t hi[3]) {
  float flo[3], fhi[3];
  int i;
  if (!LeafSolid(node)) { return; }
  for (i=0;i<3;i++) {
    flo[i] = lo[i] / 256.0f;
    fhi[i] = hi[i] / 256.0f;
  }
  EmitBox((tri_sink*)ctx, flo, fhi, CRASHCRAFT_TRI_SOLID);
}

static void SolidZone(entry *zone, void *ctx) {
  ZoneWalk(zone, SolidLeaf, ctx);
}

int crashcraft_game_static_tris(crashcraft_tri *out, int max) {
  tri_sink sink;
  sink.out = out; sink.max = max; sink.count = 0; sink.boxes = 0;
  ForEachZone(SolidZone, &sink);
  return sink.count;
}

/* hazards: the leaf containing a point, in any zone */
typedef struct {
  int32_t p[3];
  uint32_t event;
} hazard_query;

static void HazardLeaf(void *ctx, uint16_t node, const int32_t lo[3], const int32_t hi[3]) {
  hazard_query *q = (hazard_query*)ctx;
  uint32_t ev;
  if (q->p[0] < lo[0] || q->p[0] >= hi[0] || q->p[1] < lo[1] || q->p[1] >= hi[1]
   || q->p[2] < lo[2] || q->p[2] >= hi[2])
    return;
  ev = LeafHazard(node);
  if (ev) { q->event = ev; }
}

static void HazardZone(entry *zone, void *ctx) {
  hazard_query *q = (hazard_query*)ctx;
  zone_rect *zr = (zone_rect*)zone->items[1];
  if (q->p[0] < (zr->x << 8) || q->p[0] >= ((zr->x + (int32_t)zr->w) << 8)
   || q->p[1] < (zr->y << 8) || q->p[1] >= ((zr->y + (int32_t)zr->h) << 8)
   || q->p[2] < (zr->z << 8) || q->p[2] >= ((zr->z + (int32_t)zr->d) << 8))
    return;
  ZoneWalk(zone, HazardLeaf, ctx);
}

typedef struct {
  int32_t lo[3], hi[3]; /* object units */
  uint32_t event;
} level_box;

static level_box *hazards = 0, *zones = 0;
static int hazard_count = 0, hazard_cap = 0, zone_count = 0, zone_cap = 0;

static void PushBox(level_box **arr, int *n, int *cap, const int32_t lo[3], const int32_t hi[3], uint32_t ev) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 256;
    *arr = (level_box*)realloc(*arr, sizeof(level_box) * *cap);
  }
  memcpy((*arr)[*n].lo, lo, sizeof(int32_t)*3);
  memcpy((*arr)[*n].hi, hi, sizeof(int32_t)*3);
  (*arr)[*n].event = ev;
  (*n)++;
}

static void CacheLeaf(void *ctx, uint16_t node, const int32_t lo[3], const int32_t hi[3]) {
  uint32_t ev = LeafHazard(node);
  (void)ctx;
  if (ev)
    PushBox(&hazards, &hazard_count, &hazard_cap, lo, hi, ev);
}

static void CacheZone(entry *zone, void *ctx) {
  zone_rect *zr = (zone_rect*)zone->items[1];
  int32_t lo[3], hi[3];
  (void)ctx;
  lo[0] = zr->x << 8; lo[1] = zr->y << 8; lo[2] = zr->z << 8;
  hi[0] = (zr->x + (int32_t)zr->w) << 8;
  hi[1] = (zr->y + (int32_t)zr->h) << 8;
  hi[2] = (zr->z + (int32_t)zr->d) << 8;
  PushBox(&zones, &zone_count, &zone_cap, lo, hi, 0);
  ZoneWalk(zone, CacheLeaf, 0);
}

void crashcraft_game_cache_level(void) {
  hazard_count = 0;
  zone_count = 0;
  ForEachZone(CacheZone, 0);
  crashcraft_log("level cache: %d zones, %d hazard volumes", zone_count, hazard_count);
}

static int InBox(const level_box *b, const int32_t p[3]) {
  return p[0] >= b->lo[0] && p[0] < b->hi[0] && p[1] >= b->lo[1] && p[1] < b->hi[1]
      && p[2] >= b->lo[2] && p[2] < b->hi[2];
}

uint32_t crashcraft_game_hazard_at(const float pos[3]) {
  int32_t p[3];
  int i;
  p[0] = (int32_t)(pos[0] * 256.0f);
  p[1] = (int32_t)(pos[1] * 256.0f) + 0x1000; /* a little above the feet */
  p[2] = (int32_t)(pos[2] * 256.0f);
  for (i=0;i<hazard_count;i++)
    if (InBox(&hazards[i], p)) { return hazards[i].event; }
  return 0;
}

int crashcraft_game_outside_level(const float pos[3]) {
  int32_t p[3];
  int i;
  if (!zone_count) { return 0; }
  p[0] = (int32_t)(pos[0] * 256.0f);
  p[1] = (int32_t)(pos[1] * 256.0f);
  p[2] = (int32_t)(pos[2] * 256.0f);
  for (i=0;i<zone_count;i++)
    if (InBox(&zones[i], p)) { return 0; }
  return 1;
}

float crashcraft_game_water_level(float x, float y, float z) {
  (void)x; (void)y; (void)z;
  return -1.0e30f; /* Crash 1's water is a hazard volume, not swimmable */
}

/* ---- objects ------------------------------------------------------------------------------------- */

static gool_header *ObjHeader(gool_object *obj) {
  if (!obj || !obj->global || ISERRORCODE(obj->global)) { return 0; }
  return (gool_header*)obj->global->items[0];
}

static uint32_t ObjId(gool_object *obj) {
  return (uint32_t)(uintptr_t)obj; /* objects live in a fixed pool */
}

typedef struct {
  crashcraft_actor *out;
  int max, count;
  float near[3], radius;
  tri_sink *tris;          /* dynamic collision instead of actors */
} object_query;

static object_query *cur_query;

static int ObjectVisit(gool_object *obj, int arg) {
  object_query *q = cur_query;
  gool_header *header;
  crashcraft_actor *a;
  float c[3], half[3], lo[3], hi[3], dx, dz;
  int i;

  (void)arg;
  if (obj == crash || obj->handle.type != 1) { return SUCCESS; }
  if (!(obj->status_b & GOOL_FLAG_COLLIDABLE)) { return SUCCESS; }
  if (obj->status_b & GOOL_FLAG_INVISIBLE) { return SUCCESS; }
  header = ObjHeader(obj);
  if (!header) { return SUCCESS; }
  for (i=0;i<3;i++) {
    lo[i] = (((int32_t*)&obj->trans)[i] + ((int32_t*)&obj->bound.p1)[i]) / 256.0f;
    hi[i] = (((int32_t*)&obj->trans)[i] + ((int32_t*)&obj->bound.p2)[i]) / 256.0f;
    c[i] = (lo[i] + hi[i]) * 0.5f;
    half[i] = (hi[i] - lo[i]) * 0.5f;
  }
  if (half[0] <= 0 || half[1] <= 0 || half[2] <= 0) { return SUCCESS; }
  dx = c[0] - q->near[0];
  dz = c[2] - q->near[2];
  if (dx*dx + dz*dz > q->radius*q->radius || fabsf(c[1] - q->near[1]) > q->radius) { return SUCCESS; }
  if (q->tris) {
    /* things Crash stands on: boxes and anything with a solid top */
    if (header->type == BOX_TYPE || (obj->status_b & (GOOL_FLAG_SOLID_TOP | GOOL_FLAG_SOLID_SIDES)))
      EmitBox(q->tris, lo, hi, CRASHCRAFT_TRI_SOLID);
    return SUCCESS;
  }
  if (!q->out || q->count >= q->max) { return SUCCESS; }
  a = &q->out[q->count++];
  memset(a, 0, sizeof(*a));
  a->id = ObjId(obj);
  a->pos[0] = c[0]; a->pos[1] = lo[1]; a->pos[2] = c[2];
  a->half[0] = half[0]; a->half[1] = half[1]; a->half[2] = half[2];
  a->yaw_deg = 180.0f - (obj->rot.x & 0xFFF) * 360.0f / 4096.0f;
  a->kind = header->type == BOX_TYPE ? CRASHCRAFT_ACTOR_BOX : CRASHCRAFT_ACTOR_ENEMY;
  a->hostile = a->kind == CRASHCRAFT_ACTOR_ENEMY;
  snprintf(a->name, sizeof(a->name), "%s", NSEIDToString(obj->global->eid));
  return SUCCESS;
}

static void VisitObjects(object_query *q) {
  int i;
  cur_query = q;
  for (i=0;i<8;i++)
    GoolObjectHandleTraverseTreePreorder((gool_object*)&handles[i], ObjectVisit, 0);
  cur_query = 0;
}

int crashcraft_game_actors(crashcraft_actor *out, int max, const float near[3], float radius) {
  object_query q;
  if (!crashcraft_game_in_level()) { return 0; }
  memset(&q, 0, sizeof(q));
  q.out = out; q.max = max; q.radius = radius;
  memcpy(q.near, near, sizeof(q.near));
  VisitObjects(&q);
  return q.count;
}

int crashcraft_game_dynamic_tris(crashcraft_tri *out, int max, const float near[3], float radius) {
  object_query q;
  tri_sink sink;
  if (!crashcraft_game_in_level()) { return 0; }
  memset(&q, 0, sizeof(q));
  sink.out = out; sink.max = max; sink.count = 0; sink.boxes = 0;
  q.radius = radius;
  q.tris = &sink;
  memcpy(q.near, near, sizeof(q.near));
  VisitObjects(&q);
  return sink.count;
}

static gool_object *FindObject(uint32_t id) {
  object_query q;
  /* ids are object addresses; only trust ones still in the live tree */
  crashcraft_actor found[256];
  int i, n;
  float near[3] = { 0, 0, 0 };
  memset(&q, 0, sizeof(q));
  q.out = found; q.max = 256; q.radius = 1.0e9f;
  memcpy(q.near, near, sizeof(near));
  VisitObjects(&q);
  n = q.count;
  for (i=0;i<n;i++)
    if (found[i].id == id) { return (gool_object*)(uintptr_t)id; }
  return 0;
}

static int BounceCrate(gool_object *obj);
static void CrashMove(gool_object *obj, uint32_t event);

void crashcraft_game_hit_actor(uint32_t id, float damage, int from_above) {
  gool_object *obj;
  gool_header *header;
  uint32_t event;
  (void)damage;
  if (!crashcraft_game_in_level()) { return; }
  obj = FindObject(id);
  if (!obj) {
    crashcraft_log("hit: object %08x is gone", id);
    return;
  }
  crashcraft_log("hit: %s subtype %u (%s)", NSEIDToString(obj->global->eid), obj->subtype, from_above ? "from above" : "spin");
  /* Crash's crates ignore a spin unless Crash is really spinning, but always answer his jump:
     breakable crates take the jump (fruit, ?, life, checkpoint, TNT's fuse); bounce crates the
     spin, so hitting one doesn't throw Steve */
  header = ObjHeader(obj);
  if (header && header->type == BOX_TYPE && !BounceCrate(obj))
    event = GOOL_EVENT_JUMPED_ON;
  else
    event = from_above ? GOOL_EVENT_JUMPED_ON : GOOL_EVENT_SPIN_HIT;
  CrashMove(obj, event);
  crashcraft_log("hit: sent %x, %s", event, obj->handle.type == 1 && obj->global ? "still there" : "gone");
}

void crashcraft_game_kill_crash(uint32_t event) {
  if (!crashcraft_game_in_level()) { return; }
  takeover_frames = 30;
  g_crashcraft_puppet.active = 0;
  crash->status_b &= ~GOOL_FLAG_INVISIBLE;
  GoolSendEvent(0, crash, event ? event : GOOL_EVENT_FALL_KILL, 0, 0);
}

/* crate subtypes that bounce Crash instead of breaking when he lands on them (found with
   CRASHCRAFT_PROBE: they answer Crash's jump with GOOL_EVENT_BOUNCE and stay) */
static int BounceCrate(gool_object *obj) {
  return obj->subtype == 3;
}

/* Crash's move on an object, sent the way Crash's own code would: a jump comes from above (the
   crate's script checks where Crash is and that he's falling). Crash goes back after. */
static gool_object *move_target = 0; /* Crash stays on it for a few frames: its script checks */
static int move_frames = 0;
static uint32_t move_event = 0;

static void CrashOnTarget(void) {
  gool_object *obj = move_target;
  if (move_event == GOOL_EVENT_JUMPED_ON) {
    crash->trans.x = obj->trans.x;
    crash->trans.z = obj->trans.z;
    crash->trans.y = obj->trans.y + 0x19000;
    crash->velocity.x = 0;
    crash->velocity.z = 0;
    crash->velocity.y = -0x8000;
  }
  else {
    crash->trans.x = obj->trans.x;
    crash->trans.z = obj->trans.z + obj->bound.p2.z + 0x4000;
    crash->trans.y = obj->trans.y + obj->bound.p1.y;
  }
}

static void CrashMove(gool_object *obj, uint32_t event) {
  gool_header *header = ObjHeader(obj);
  if (BounceCrate(obj) || !header || header->type != BOX_TYPE) {
    GoolSendEvent(crash, obj, event, 0, 0); /* enemies; bounce crates answer with GOOL_EVENT_BOUNCE */
    return;
  }
  /* a crate breaks when Crash really lands on it: for a few frames his own code runs (hidden),
     dropped onto the crate from just above, as CRASHCRAFT_PROBE showed breaks every breakable one */
  move_target = obj;
  move_event = event;
  move_frames = 12;
  CrashOnTarget();
  GoolSendEvent(crash, obj, event, 0, 0);
}

static float launch_pending = 0;
static uint32_t last_bounce_frame = 0;

void crashcraft_game_landed(const float feet[3]) {
  object_query q;
  crashcraft_actor found[64];
  gool_object *obj;
  float top;
  int i;
  if (!crashcraft_game_in_level()) { return; }
  memset(&q, 0, sizeof(q));
  q.out = found; q.max = 64; q.radius = 800.0f;
  memcpy(q.near, feet, sizeof(q.near));
  VisitObjects(&q);
  for (i=0;i<q.count;i++) {
    if (found[i].kind != CRASHCRAFT_ACTOR_BOX) { continue; }
    top = found[i].pos[1] + found[i].half[1]*2;
    if (fabsf(feet[1] - top) > 60.0f) { continue; }            /* standing on its top */
    if (fabsf(feet[0] - found[i].pos[0]) > found[i].half[0] + 40.0f) { continue; }
    if (fabsf(feet[2] - found[i].pos[2]) > found[i].half[2] + 40.0f) { continue; }
    obj = (gool_object*)(uintptr_t)found[i].id;
    crashcraft_log("landed on crate %s subtype %u%s", found[i].name, obj->subtype, BounceCrate(obj) ? " (bounce)" : "");
    if (BounceCrate(obj))
      CrashMove(obj, GOOL_EVENT_JUMPED_ON);
    return;
  }
}

/* Steve rising into a crate from below (a bounce under a floating crate): it breaks as from
   Crash's head (bounce crates only get a spin) */
void crashcraft_game_head_bump(const float feet[3], float head_height) {
  static uint32_t last_frame = 0;
  object_query q;
  crashcraft_actor found[64];
  gool_object *obj;
  float head, bottom;
  int i;
  if (!crashcraft_game_in_level() || frames_elapsed - last_frame < 10) { return; }
  head = feet[1] + head_height;
  memset(&q, 0, sizeof(q));
  q.out = found; q.max = 64; q.radius = 1200.0f;
  memcpy(q.near, feet, sizeof(q.near));
  VisitObjects(&q);
  for (i=0;i<q.count;i++) {
    if (found[i].kind != CRASHCRAFT_ACTOR_BOX) { continue; }
    bottom = found[i].pos[1];
    if (bottom - head < -60.0f || bottom - head > 80.0f) { continue; }
    if (fabsf(feet[0] - found[i].pos[0]) > found[i].half[0] + 100.0f) { continue; }
    if (fabsf(feet[2] - found[i].pos[2]) > found[i].half[2] + 100.0f) { continue; }
    obj = (gool_object*)(uintptr_t)found[i].id;
    last_frame = frames_elapsed;
    crashcraft_log("head bump: crate %s subtype %u", found[i].name, obj->subtype);
    CrashMove(obj, BounceCrate(obj) ? GOOL_EVENT_SPIN_HIT : GOOL_EVENT_JUMPED_ON);
    return;
  }
}

float crashcraft_game_take_launch(void) {
  float v = launch_pending;
  launch_pending = 0;
  return v;
}

int crashcraft_game_take_damage(void) {
  int d = damage_pending;
  damage_pending = 0;
  return d;
}

/* GoolSendEvent hook: events for Crash while Steve plays. Returns 1 to drop the event. */
static int probe_logging = 0;

int crashcraft_game_event_to_crash(void *_sender, uint32_t event) {
  gool_object *sender = (gool_object*)_sender;
  gool_header *header;
  if (probe_logging)
    crashcraft_log("probe:   -> event %x for Crash from %s (subtype %d)", event,
      sender && sender->global ? NSEIDToString(sender->global->eid) : "?", sender ? (int)sender->subtype : -1);
  if (!g_crashcraft_puppet.active) { return 0; }
  header = ObjHeader(sender);
  switch (event) {
  case GOOL_EVENT_HIT:
  case GOOL_EVENT_PLAYER_DAMAGE:
  case GOOL_EVENT_SQUASH:
  case GOOL_EVENT_BOULDER_SQUASH:
  case GOOL_EVENT_EXPLODE:
  case GOOL_EVENT_BURN:
  case GOOL_EVENT_SHOCK:
    /* Crash dies on the first touch, so enemies keep sending hits every frame: Steve gets
       Minecraft-style invulnerability (one hit per second) */
    if (frames_elapsed - last_hurt_frame < 30) { return 1; }
    last_hurt_frame = frames_elapsed;
    damage_pending++;
    crashcraft_log("%s hurt the player (event %x)", sender && sender->global ? NSEIDToString(sender->global->eid) : "world", event);
    return 1;
  case GOOL_EVENT_FALL_KILL:
  case GOOL_EVENT_DROWN:
    return 1; /* hazard volumes are checked against Steve's own position */
  default:
    break;
  }
  if (event == GOOL_EVENT_BOUNCE && header && header->type == BOX_TYPE) {
    /* a bounce crate throws Crash up: Steve too (once; the crate repeats it for a few frames) */
    if (frames_elapsed - last_bounce_frame > 10) {
      last_bounce_frame = frames_elapsed;
      launch_pending = 0.8f; /* blocks per tick: about 4 blocks high */
      crashcraft_log("bounce crate %s threw the player up", NSEIDToString(sender->global->eid));
    }
    return 1;
  }
  if (event >= 0x100 && header && header->type == BOX_TYPE)
    return 1; /* other crate reactions Crash's code would run: Steve has his own physics */
  crashcraft_log("event %x for Crash from %s", event, sender && sender->global ? NSEIDToString(sender->global->eid) : "?");
  switch (event) {
  case 0x200: case 0x1100: case 0x2400: case 0x2a00: /* fruit, lives */
  case 0x2800:                                         /* Aku Aku */
  case GOOL_EVENT_COMBO:                               /* enemy defeated */
  case GOOL_EVENT_TERMINATE:
    /* a checkpoint crate tells Crash to save the level state: his own code does the saving */
    crash_code_frames = 4;
    break;
  case GOOL_EVENT_RESPAWN:
  case GOOL_EVENT_STATUS:
    break;
  default:
    /* warps, level end, scripted moments: Crash's own code plays them out */
    CrashTakesOver("an event only Crash's code handles", 300);
    break;
  }
  return 0;
}

int crashcraft_game_touches_crash(void *_obj) {
  gool_header *header;
  if (!g_crashcraft_puppet.active) { return 1; }
  if (move_frames > 0 && _obj == move_target && !BounceCrate(move_target))
    return 1; /* the crate Crash is breaking (a bounce crate bounces only while he isn't on it) */
  header = ObjHeader((gool_object*)_obj);
  /* a box Steve walks into or stands on stays whole: he breaks boxes by hitting them */
  return !(header && header->type == BOX_TYPE);
}

/* ---- level end ------------------------------------------------------------------------------------ */

/* c1 can't load the level-complete screen (LID_LEVELEND) from the disc's files (it crashes in NSInit,
   with or without CrashCraft), so a finished level goes straight to the next one on Crash 1's route. */
static const lid_t level_route[] = {
  LID_NSANITYBEACH, LID_JUNGLEROLLERS, LID_THEGREATGATE, LID_BOULDERS, LID_UPSTREAM, LID_PAPUPAPU,
  LID_ROLLINGSTONES, LID_HOGWILD, LID_NATIVEFORTRESS, LID_UPTHECREEK, LID_RIPPERROO,
  LID_THELOSTCITY, LID_TEMPLERUINS, LID_ROADTONOWHERE, LID_BOULDERDASH, LID_SUNSETVISTA, LID_KOALAKONG,
  LID_HEAVYMACHINERY, LID_CORTEXPOWER, LID_GENERATORROOM, LID_TOXICWASTE, LID_PINSTRIPE,
  LID_THEHIGHROAD, LID_SLIPPERYCLIMB, LID_LIGHTSOUT, LID_JAWSOFDARKNESS, LID_CASTLEMACHINERY,
  LID_DRNBRIO, LID_THELAB, LID_THEGREATHALL, LID_DRNEOCORTEX,
};

/* secret levels rejoin the route where you'd find them */
static lid_t SecretNext(lid_t lid) {
  switch (lid) {
  case LID_WHOLEHOG: return LID_SUNSETVISTA;
  case LID_FUMBLINGINTHEDARK: return LID_JAWSOFDARKNESS;
  case LID_STORMYASCENT: return LID_CASTLEMACHINERY;
  default: return -1;
  }
}

int crashcraft_game_next_level(int _next) {
  lid_t next = (lid_t)_next;
  int i, n;
  lid_t to;
  if (next != LID_LEVELEND) { return next; }
  to = SecretNext(cur_lid);
  n = sizeof(level_route) / sizeof(level_route[0]);
  for (i=0;i<n && to == (lid_t)-1;i++)
    if (level_route[i] == cur_lid)
      to = i + 1 < n ? level_route[i+1] : LID_TITLE;
  if (to == (lid_t)-1) { to = LID_TITLE; }
  checkpoint_id = -1; /* the new level starts at its beginning */
  crashcraft_log("level %x finished: next level %x (level-complete screen skipped)", (int)cur_lid, (int)to);
  return to;
}

/* The level ends on WarpC, the warp pad: it reacts to Crash himself standing on it (his own code plays
   the warp-out and changes level). When Steve steps onto it, Crash takes over from that spot. */
static gool_object *found_warp;

static int FindWarpVisit(gool_object *obj, int arg) {
  (void)arg;
  if (obj->handle.type != 1 || !obj->global || ISERRORCODE(obj->global)) { return SUCCESS; }
  if (strcmp(NSEIDToString(obj->global->eid), "WarpC") == 0)
    found_warp = obj;
  return SUCCESS;
}

static int OnWarpPad(const float feet[3]) {
  int k;
  float dx, dy, dz;
  found_warp = 0;
  for (k=0;k<8;k++)
    GoolObjectHandleTraverseTreePreorder((gool_object*)&handles[k], FindWarpVisit, 0);
  if (!found_warp) { return 0; }
  dx = feet[0] - found_warp->trans.x / 256.0f;
  dy = feet[1] - found_warp->trans.y / 256.0f;
  dz = feet[2] - found_warp->trans.z / 256.0f;
  return dx*dx + dz*dz < 400.0f*400.0f && dy > -200.0f && dy < 800.0f;
}

static void CrashTakesOver(const char *why, int frames) {
  if (takeover_frames < frames)
    takeover_frames = frames;
  if (g_crashcraft_puppet.active)
    crashcraft_log("Crash takes over: %s", why);
  g_crashcraft_puppet.active = 0;
  if (crash)
    crash->status_b &= ~GOOL_FLAG_INVISIBLE;
}

/* ---- the puppet ---------------------------------------------------------------------------------- */

static vec saved_cam_trans;
static ang saved_cam_rot;
static uint32_t saved_screen_proj;
static int cam_overridden = 0;

/* CRASHCRAFT_PROBE=1: c1 alone finds every crate entity in the level's zones, spawns one of each
   subtype twice and sends the first Crash's jump, the second Crash's spin, logging how each reacts
   (dies, events for Crash). Tells the bounce/iron/TNT crates apart. */
typedef struct {
  entry *zone;
  int idx;
  int subtype;
  int count;
} probe_kind;

static probe_kind probe_kinds[64];
static int probe_kind_count = 0;

static void ProbeZone(entry *zone, void *ctx) {
  zone_header *header;
  zone_entity *entity;
  int i, k, idx;
  (void)ctx;
  header = (zone_header*)zone->items[0];
  for (i=0;i<(int)header->entity_count;i++) {
    idx = header->paths_idx + header->path_count + i;
    entity = (zone_entity*)zone->items[idx];
    if (entity->type != BOX_TYPE) { continue; }
    for (k=0;k<probe_kind_count;k++)
      if (probe_kinds[k].subtype == entity->subtype) { break; }
    if (k == probe_kind_count) {
      if (probe_kind_count == 64) { continue; }
      probe_kinds[k].zone = zone;
      probe_kinds[k].idx = i;
      probe_kinds[k].subtype = entity->subtype;
      probe_kinds[k].count = 0;
      probe_kind_count++;
    }
    probe_kinds[k].count++;
  }
}

/* CRASHCRAFT_PROBE=list: every entity type in the level's zones, with its GOOL program and where */
static void ListZone(entry *zone, void *ctx) {
  zone_header *header;
  zone_entity *entity;
  zone_rect *zr;
  eid_t exec;
  int i, idx;
  (void)ctx;
  header = (zone_header*)zone->items[0];
  zr = (zone_rect*)zone->items[1];
  for (i=0;i<(int)header->entity_count;i++) {
    idx = header->paths_idx + header->path_count + i;
    entity = (zone_entity*)zone->items[idx];
    exec = ns.ldat->exec_map[entity->type];
    crashcraft_log("list: zone %s (z %d..%d) entity id %d type %x subtype %d at %d %d %d", NSEIDToString(zone->eid),
      zr->z, zr->z + (int32_t)zr->d, entity->id, entity->type, entity->subtype,
      entity->loc.x, entity->loc.y, entity->loc.z);
    if (entity->type != BOX_TYPE && entity->type != 0x16 && entity->type != 0x13) {
      gool_object *obj = GoolObjectSpawn(zone, i);
      if (!ISERRORCODE(obj) && obj)
        crashcraft_log("list:   spawned %s at %d %d %d (status_b %x)", obj->global ? NSEIDToString(obj->global->eid) : "?",
          obj->trans.x >> 8, obj->trans.y >> 8, obj->trans.z >> 8, obj->status_b);
    }
  }
}

static void FindWarp(entry *zone, void *ctx) {
  gool_object **out = (gool_object**)ctx;
  zone_header *header;
  zone_entity *entity;
  int i;
  if (*out) { return; }
  header = (zone_header*)zone->items[0];
  for (i=0;i<(int)header->entity_count;i++) {
    entity = (zone_entity*)zone->items[header->paths_idx + header->path_count + i];
    if (entity->type == 0x20) {
      *out = GoolObjectSpawn(zone, i);
      if (ISERRORCODE(*out)) { *out = 0; }
      return;
    }
  }
}

static void ProbeBoxes(void) {
  static int frame = 0, idx = 0, count = 0, inited = 0;
  static gool_object *boxes[128];
  static int subtypes[128], spin[128];
  static const char *mode = 0;
  gool_object *box;
  int i, n, alive;

  if (!inited) {
    mode = SDL_getenv("CRASHCRAFT_PROBE");
    inited = 1;
  }
  if (!mode || !crashcraft_game_in_level()) { return; }
  frame++;
  if (frame == 90 && strcmp(mode, "list") == 0) {
    ForEachZone(ListZone, 0);
    return;
  }
  if (strcmp(mode, "list") == 0) { return; }
  if (strcmp(mode, "levelend") == 0) { /* the level-end transition, without CrashCraft involved */
    if (frame == 100) { crashcraft_log("probe: next_lid = level end"); next_lid = LID_LEVELEND; }
    return;
  }
  if (strcmp(mode, "warp") == 0) {
    /* the level-end pad: spawn it, drop Crash onto it, watch what happens */
    static gool_object *warp = 0;
    static uint32_t last_state = 0;
    if (frame == 90) {
      ForEachZone(FindWarp, &warp);
      if (warp) {
        crash->trans = warp->trans;
        crash->trans.y += 0x19000;
        crash->velocity.y = 0;
        probe_logging = 1;
        crashcraft_log("probe: WarpC at %d %d %d, bound y %d..%d, status_b %x; Crash dropped on it", warp->trans.x >> 8,
          warp->trans.y >> 8, warp->trans.z >> 8, warp->bound.p1.y >> 8, warp->bound.p2.y >> 8, warp->status_b);
      }
    }
    if (frame > 90 && frame < 400 && warp) {
      if (crash->state != last_state || (frame % 30) == 0) {
        crashcraft_log("probe: frame %d crash state %x at %d %d %d, status_a %x, collider %s, warp state %x, next_lid %d",
          frame, crash->state, crash->trans.x >> 8, crash->trans.y >> 8, crash->trans.z >> 8, crash->status_a,
          crash->collider && crash->collider->global ? NSEIDToString(crash->collider->global->eid) : "-", warp->state, (int)next_lid);
        last_state = crash->state;
      }
    }
    return;
  }
  if (frame == 90) {
    ForEachZone(ProbeZone, 0);
    for (i=0;i<probe_kind_count;i++) {
      crashcraft_log("probe: crate subtype %d x%d", probe_kinds[i].subtype, probe_kinds[i].count);
      for (n=0;n<2 && count<128;n++) {
        box = GoolObjectSpawn(probe_kinds[i].zone, probe_kinds[i].idx);
        if (ISERRORCODE(box) || !box) {
          crashcraft_log("probe:   spawn failed (%d)", (int)box);
          break;
        }
        boxes[count] = box;
        subtypes[count] = probe_kinds[i].subtype;
        spin[count] = n;
        count++;
      }
    }
  }
  if (frame < 100 || idx >= count) { return; }
  box = boxes[idx];
  if ((frame - 100) % 20 == 0) {
    crashcraft_log("probe: crate subtype %d (%s) at %d %d %d, state %x: Crash's %s", subtypes[idx],
      box->global ? NSEIDToString(box->global->eid) : "?", box->trans.x >> 8, box->trans.y >> 8, box->trans.z >> 8,
      box->state, spin[idx] ? "spin" : "jump");
    probe_logging = 1;
    crash->trans = box->trans;
    if (spin[idx])
      GoolSendEvent(crash, box, GOOL_EVENT_SPIN_HIT, 0, 0);
    else {
      crash->trans.y += 0x19000;
      crash->velocity.y = -0x8000;
      GoolSendEvent(crash, box, GOOL_EVENT_JUMPED_ON, 0, 0);
    }
  }
  else if ((frame - 100) % 20 == 15) {
    alive = box->handle.type == 1 && (int)box->subtype == subtypes[idx];
    crashcraft_log("probe:   -> %s, state %x", alive ? "still there" : "GONE", box->state);
    probe_logging = 0;
    idx++;
  }
}

void crashcraft_game_puppet_before(void) {
  crashcraft_puppet *pp = &g_crashcraft_puppet;
  ProbeBoxes();
  if (takeover_frames > 0) { takeover_frames--; }
  if (crash_code_frames > 0) { crash_code_frames--; }
  {
    static vec saved = { 0, 0, 0 };
    if (memcmp(&saved, &savestate.player_trans, sizeof(vec)) != 0) {
      saved = savestate.player_trans;
      crashcraft_log("checkpoint: respawn point now %d %d %d (zone %s)", saved.x >> 8, saved.y >> 8, saved.z >> 8,
        NSEIDToString(savestate.zone));
    }
  }
  if (!crash || !pp->active) { return; }
  if (OnWarpPad(pp->pos)) {
    /* put Crash exactly where Steve stands, then let him warp out (several seconds) */
    crash->trans.x = (int32_t)(pp->pos[0] * 256.0f);
    crash->trans.y = (int32_t)(pp->pos[1] * 256.0f);
    crash->trans.z = (int32_t)(pp->pos[2] * 256.0f);
    crash->velocity.x = crash->velocity.z = 0;
    CrashTakesOver("Steve reached the warp pad (level end)", 600);
    return;
  }
  if (move_frames > 0) {
    move_frames--;
    if (move_target && move_target->handle.type == 1)
      return; /* Crash's own code is landing on the crate */
    move_frames = 0;
  }
  crash->trans.x = (int32_t)(pp->pos[0] * 256.0f);
  crash->trans.y = (int32_t)(pp->pos[1] * 256.0f);
  crash->trans.z = (int32_t)(pp->pos[2] * 256.0f);
  crash->velocity.x = (int32_t)(pp->vel[0] * 256.0f);
  crash->velocity.y = (int32_t)(pp->vel[1] * 256.0f);
  crash->velocity.z = (int32_t)(pp->vel[2] * 256.0f);
  crash->rot.x = DEG_TO_ANG(180.0f - pp->yaw_deg);
  /* objects only test against Crash when he was "animated" this frame */
  crash->anim_stamp = context.draw_stamp / 34;
}

void crashcraft_game_puppet_after(void) {
  if (!crash || !g_crashcraft_puppet.active) { return; }
  if (move_frames > 0) { return; }
  crash->trans.x = (int32_t)(g_crashcraft_puppet.pos[0] * 256.0f);
  crash->trans.y = (int32_t)(g_crashcraft_puppet.pos[1] * 256.0f);
  crash->trans.z = (int32_t)(g_crashcraft_puppet.pos[2] * 256.0f);
}

/* GoolObjectUpdate hook: Crash while puppeted runs no code and isn't drawn */
int crashcraft_game_skip_object(void *obj) {
  if (obj != crash || !g_crashcraft_puppet.active) { return 0; }
  if (crash_code_frames > 0) { return 0; } /* checkpoint: his code saves the level state */
  if (move_frames > 0) { return 0; } /* breaking a crate: his code runs (he stays hidden) */
  ((gool_object*)obj)->anim_stamp = frames_elapsed;
  return 1;
}

int crashcraft_game_hide_object(const void *obj) {
  return obj == crash && g_crashcraft_puppet.active;
}

/* ---- the camera ---------------------------------------------------------------------------------- */

/* every polygon of the zone's worlds (the camera isn't on Crash's rail, so its precomputed
   visibility lists don't apply; the depth buffer sorts) */
static poly_id_list *all_polys = 0;
static poly_id_list *saved_poly_ids = 0;

static poly_id_list *AllPolys(void) {
  zone_header *header;
  wgeo_header *w;
  entry *wgeo;
  poly_id *ids;
  int i, j, n;

  if (!all_polys)
    all_polys = (poly_id_list*)malloc(sizeof(poly_id_list) + sizeof(poly_id)*0x10000);
  header = (zone_header*)cur_zone->items[0];
  ids = all_polys->ids;
  n = 0;
  for (i=0;i<(int)header->world_count && i<8;i++) {
    wgeo = NSLookup(&header->worlds[i].eid);
    if (ISERRORCODE(wgeo) || !wgeo) { continue; }
    w = (wgeo_header*)wgeo->items[0];
    for (j=0;j<(int)w->poly_count && j<0x1000 && n<0xFFFF;j++) {
      ids[n].flag = 0;
      ids[n].world_idx = i;
      ids[n].poly_idx = j;
      n++;
    }
  }
  all_polys->len = n;
  all_polys->type = 0;
  return all_polys;
}

/* main.c, after CamUpdate and before GfxUpdateMatrices: Steve's eye replaces Crash's camera */
void crashcraft_game_camera_begin(void) {
  crashcraft_camera *cam = &g_crashcraft_camera;
  float t;
  if (!cam->active || !crashcraft_game_in_level()) { return; }
  saved_cam_trans = cam_trans;
  saved_cam_rot = cam_rot;
  saved_screen_proj = screen_proj;
  cam_trans.x = (int32_t)(cam->pos[0] * 256.0f);
  cam_trans.y = (int32_t)(cam->pos[1] * 256.0f);
  cam_trans.z = (int32_t)(cam->pos[2] * 256.0f);
  cam_rot.x = DEG_TO_ANG(180.0f - cam->yaw_deg);
  cam_rot.y = DEG_TO_ANG(-cam->pitch_deg);
  cam_rot.z = 0;
  /* vertical fov: 120 px half height over the 5/8-scaled y: proj = 192 / tan(fov/2) */
  t = tanf(cam->fov_deg * 0.5f * 3.14159265f / 180.0f);
  if (t > 0.01f)
    screen_proj = (uint32_t)lroundf(192.0f / t);
  saved_poly_ids = cur_poly_ids;
  cur_poly_ids = AllPolys();
  cam_overridden = 1;
}

/* after the frame is drawn: Crash's camera code keeps its own state */
void crashcraft_game_camera_end(void) {
  if (!cam_overridden) { return; }
  cam_trans = saved_cam_trans;
  cam_rot = saved_cam_rot;
  screen_proj = saved_screen_proj;
  cur_poly_ids = saved_poly_ids;
  cam_overridden = 0;
}

/* c1 render units -> clip space, exactly as gl.c draws homogeneous verts (see GLDrawPrims) */
int crashcraft_game_view_proj(float out[16]) {
  const float K = 4.0f, n = 1.0f, f = 100000.0f;
  float m[3][3], e[3][4], c[3], offx, offy, proj, fr[4][4], mvp[4][4];
  int i, j, k;

  for (i=0;i<3;i++)
    for (j=0;j<3;j++)
      m[i][j] = ms_cam_rot.m[i][j] / 4096.0f;
  c[0] = (float)(cam_trans.x >> 8);
  c[1] = (float)(cam_trans.y >> 8);
  c[2] = (float)(cam_trans.z >> 8);
  offx = (float)params.screen.x;
  offy = (float)params.screen.y;
  proj = (float)params.screen_proj;
  /* eye = (h.x, -h.y, -h.z) / K, h = (offx*rz + proj*rx, offy*rz + proj*ry, rz), r = M (p - c) */
  for (j=0;j<3;j++) {
    e[0][j] = ( offx*m[2][j] + proj*m[0][j]) / K;
    e[1][j] = -(offy*m[2][j] + proj*m[1][j]) / K;
    e[2][j] = -m[2][j] / K;
  }
  for (i=0;i<3;i++)
    e[i][3] = -(e[i][0]*c[0] + e[i][1]*c[1] + e[i][2]*c[2]);
  /* glFrustum(screen.x, screen.x+screen.w, -120, 120, n, f) (gl.c) */
  memset(fr, 0, sizeof(fr));
  fr[0][0] = 2*n/(float)screen.w;
  fr[0][2] = (2.0f*screen.x + screen.w) / (float)screen.w;
  fr[1][1] = 2*n/240.0f;
  fr[2][2] = -(f+n)/(f-n);
  fr[2][3] = -2*f*n/(f-n);
  fr[3][2] = -1;
  for (i=0;i<4;i++) {
    for (j=0;j<4;j++) {
      float s = 0;
      for (k=0;k<3;k++)
        s += fr[i][k] * e[k][j];
      if (j == 3) { s += fr[i][3]; }
      mvp[i][j] = s;
    }
  }
  for (i=0;i<4;i++)
    for (j=0;j<4;j++)
      out[j*4+i] = mvp[i][j]; /* column-major */
  return 1;
}

void crashcraft_game_drawable_size(int *w, int *h) {
  if (window)
    SDL_GL_GetDrawableSize(window, w, h);
  else {
    *w = 1024; *h = 768;
  }
}

void crashcraft_game_debug_state(char *buf, int len) {
  snprintf(buf, len, "lid %x, zone %s, in level %d, paused %d, takeover %d, crash %s state %x",
    (int)cur_lid, cur_zone ? NSEIDToString(cur_zone->eid) : "-", crashcraft_game_in_level(),
    paused, crashcraft_game_in_level() ? crashcraft_game_takeover() : -1,
    crash ? "yes" : "no", crash ? crash->state : 0);
}

/* ---- debug (test commands) ---------------------------------------------------------------------- */

static const float *cur_debug_pos;
static gool_header *ObjHeader(gool_object *obj);
static uint32_t ObjId(gool_object *obj);

/* debug: every octree leaf (any zone) containing a point, with type/subtype */
static void DebugLeaf(void *ctx, uint16_t node, const int32_t lo[3], const int32_t hi[3]) {
  int32_t *p = (int32_t*)ctx;
  if (p[0] < lo[0] || p[0] >= hi[0] || p[1] < lo[1] || p[1] >= hi[1] || p[2] < lo[2] || p[2] >= hi[2])
    return;
  crashcraft_log("cell: node %04x type %d subtype %d solid %d, y %d..%d (%d tall)", node, (node & 0xE) >> 1,
    (node & 0x3F0) >> 4, LeafSolid(node), lo[1] >> 8, hi[1] >> 8, (hi[1] - lo[1]) >> 8);
}

static void DebugZone(entry *zone, void *ctx) {
  ZoneWalk(zone, DebugLeaf, ctx);
}

static int DebugObject(gool_object *obj, int arg) {
  float *p = (float*)cur_debug_pos;
  gool_header *header;
  float c[3];
  int i;
  (void)arg;
  if (obj->handle.type != 1) { return SUCCESS; }
  for (i=0;i<3;i++)
    c[i] = ((int32_t*)&obj->trans)[i] / 256.0f;
  if (fabsf(c[0]-p[0]) > 1600 || fabsf(c[1]-p[1]) > 1600 || fabsf(c[2]-p[2]) > 1600) { return SUCCESS; }
  header = ObjHeader(obj);
  crashcraft_log("object %s type %x subtype %u at %.0f %.0f %.0f bound y %d..%d xz %d..%d status_b %x%s",
    obj->global ? NSEIDToString(obj->global->eid) : "?", header ? header->type : 0, obj->subtype, c[0], c[1], c[2],
    obj->bound.p1.y >> 8, obj->bound.p2.y >> 8, obj->bound.p1.x >> 8, obj->bound.p2.x >> 8, obj->status_b,
    obj == crash ? " (Crash)" : "");
  return SUCCESS;
}

void crashcraft_game_debug_event(const char *name, uint32_t event) {
  crashcraft_actor found[256];
  object_query q;
  gool_object *obj, *best = 0;
  float d, bestd = 1e30f, near[3];
  int i;
  crashcraft_game_crash_pos(near, 0);
  memset(&q, 0, sizeof(q));
  q.out = found; q.max = 256; q.radius = 1.0e9f;
  memcpy(q.near, near, sizeof(near));
  VisitObjects(&q);
  for (i=0;i<q.count;i++) {
    if (strcmp(found[i].name, name) != 0) { continue; }
    d = fabsf(found[i].pos[0]-near[0]) + fabsf(found[i].pos[1]-near[1]) + fabsf(found[i].pos[2]-near[2]);
    if (d < bestd) { bestd = d; best = (gool_object*)(uintptr_t)found[i].id; }
  }
  if (!best) { crashcraft_log("event: no %s nearby", name); return; }
  obj = best;
  if (event == 0xFFFF) { /* "hit": exactly what a Minecraft attack does */
    crashcraft_game_hit_actor(ObjId(obj), 1.0f, 0);
    return;
  }
  GoolSendEvent(crash, obj, event, 0, 0);
  crashcraft_log("event: sent %x to %s (subtype %u, %.0f away): state now %x, %s", event, name, obj->subtype, bestd,
    obj->state, obj->handle.type == 1 ? "alive" : "gone");
}

/* test "tp": make the zone at a point current (its first path), as walking there would */
static entry *jump_zone;
static int32_t jump_p[3];

static void JumpZoneVisit(entry *zone, void *ctx) {
  zone_rect *zr = (zone_rect*)zone->items[1];
  (void)ctx;
  if (jump_zone) { return; }
  if (jump_p[0] >= (zr->x << 8) && jump_p[0] < ((zr->x + (int32_t)zr->w) << 8)
   && jump_p[1] >= (zr->y << 8) && jump_p[1] < ((zr->y + (int32_t)zr->h) << 8)
   && jump_p[2] >= (zr->z << 8) && jump_p[2] < ((zr->z + (int32_t)zr->d) << 8))
    jump_zone = zone;
}

void crashcraft_game_debug_jump_zone(const float pos[3]) {
  zone_header *header;
  zone_path *path;
  int i;
  jump_zone = 0;
  for (i=0;i<3;i++)
    jump_p[i] = (int32_t)(pos[i] * 256.0f);
  ForEachZone(JumpZoneVisit, 0);
  if (!jump_zone) { crashcraft_log("tp: no zone there"); return; }
  header = (zone_header*)jump_zone->items[0];
  if (!header->path_count) { crashcraft_log("tp: zone %s has no path", NSEIDToString(jump_zone->eid)); return; }
  path = (zone_path*)jump_zone->items[header->paths_idx];
  LevelUpdate(jump_zone, path, 0, 0);
  crashcraft_log("tp: zone %s is current", NSEIDToString(jump_zone->eid));
}

void crashcraft_game_debug_point(const float pos[3]) {
  int k;
  cur_debug_pos = pos;
  for (k=0;k<8;k++)
    GoolObjectHandleTraverseTreePreorder((gool_object*)&handles[k], DebugObject, 0);
  int32_t p[3];
  p[0] = (int32_t)(pos[0] * 256.0f);
  p[1] = (int32_t)(pos[1] * 256.0f);
  p[2] = (int32_t)(pos[2] * 256.0f);
  crashcraft_log("cell query at %.0f %.0f %.0f", pos[0], pos[1], pos[2]);
  ForEachZone(DebugZone, p);
}

