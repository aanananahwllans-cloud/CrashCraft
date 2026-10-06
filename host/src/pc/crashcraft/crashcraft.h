/*
 * CrashCraft: play Crash Bandicoot (c1) as a Minecraft player. Adapted from SM64Craft, which follows
 * chasmlol's SkyCraft (MIT).
 *
 * Minecraft runs hidden next to c1 and drives the player: its physics, inventory, blocks and HUD.
 * c1 renders everything. The C++ side (link, collision, renderer, input) talks to the game only
 * through the plain-C functions in this header; crashcraft_game.c implements the game half.
 *
 * Units: positions are c1 render units (object units >> 8). One Crash box = 400 units = 1 block.
 */
#ifndef CRASHCRAFT_H
#define CRASHCRAFT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- hooks c1 calls (implemented in C++, host.cpp) --------------------------------------------- */

void crashcraft_init(void);              /* after the GL context exists */
void crashcraft_log(const char *fmt, ...); /* crashcraft.log (next to the exe) and stdout */
int  crashcraft_enabled(void);
void crashcraft_frame_begin(void);       /* top of each CoreLoop pass, before objects update */
void crashcraft_render_world(void);      /* after c1's own geometry: Minecraft's world */
void crashcraft_render_overlay(void);    /* after the frame: Minecraft's hand, HUD and screens */
int  crashcraft_handle_sdl_event(const void *sdl_event); /* 1: CrashCraft took it */

/* ---- state the C++ side publishes for the game hooks ------------------------------------------- */

/* Where Steve is. Crash stays there (hidden) so Crash's objects (fruit, boxes, enemies,
   checkpoints, warps) react to Steve. */
typedef struct {
  int active;           /* Minecraft drives the player this frame */
  float pos[3];         /* feet, render units */
  float vel[3];         /* render units per frame */
  float yaw_deg;        /* Minecraft yaw */
  int on_ground;
  int landed;           /* 1 on the frame Steve lands (stomps whatever is under him) */
  int fall_speed;       /* render units per frame he was falling when he landed */
  int press_start;      /* frames left to hold Start (pause) */
} crashcraft_puppet;
extern crashcraft_puppet g_crashcraft_puppet;

/* The camera this frame: Minecraft's eye. */
typedef struct {
  int active;
  float pos[3];         /* render units */
  float yaw_deg, pitch_deg; /* Minecraft angles */
  float fov_deg;        /* vertical */
} crashcraft_camera;
extern crashcraft_camera g_crashcraft_camera;

/* ---- the game half (crashcraft_game.c) ---------------------------------------------------------- */

typedef struct {
  float v[9];           /* render units */
  uint32_t flags;       /* CRASHCRAFT_TRI_* */
} crashcraft_tri;

typedef struct {
  uint32_t id;          /* stable while the object lives */
  float pos[3];         /* feet, render units */
  float half[3];        /* half extents, render units */
  float yaw_deg;
  int hostile;
  int kind;             /* CRASHCRAFT_ACTOR_* */
  char name[24];
} crashcraft_actor;

#define CRASHCRAFT_ACTOR_ENEMY 0
#define CRASHCRAFT_ACTOR_BOX   1
#define CRASHCRAFT_ACTOR_OTHER 2

/* 0 while there is no level (title, map, loading, level end); else the level id (lid) + 1. */
int crashcraft_game_area_id(void);
/* Crash exists and the level is playing. */
int crashcraft_game_in_level(void);
/* c1's menus own the keyboard (pause, title, map). */
int crashcraft_game_input_owned(void);
/* Crash is acting on his own (death, respawn, warp, level end): Minecraft waits. */
int crashcraft_game_takeover(void);
/* Crash's position (feet) and facing, render units / degrees (Minecraft yaw). */
void crashcraft_game_crash_pos(float out[3], float *yaw_deg);
/* Every solid octree cell of every zone of the level, as triangles (render units). */
int crashcraft_game_static_tris(crashcraft_tri *out, int max);
/* Solid objects near the player: boxes, platforms (render units). */
int crashcraft_game_dynamic_tris(crashcraft_tri *out, int max, const float near[3], float radius);
/* Height of the zone's water under (x, z) or a very low number if none. */
float crashcraft_game_water_level(float x, float y, float z);
/* Death volumes (pits, deadly water, electricity...) at a point: 0 none, else the GOOL event. */
uint32_t crashcraft_game_hazard_at(const float pos[3]);
/* Rebuild the level's cached zone bounds and hazard volumes (after a level loads). */
void crashcraft_game_cache_level(void);
/* debug: log the octree leaves at a point */
void crashcraft_game_debug_point(const float pos[3]);
void crashcraft_game_debug_event(const char *name, uint32_t event);
void crashcraft_game_debug_jump_zone(const float pos[3]);
/* test command "pad": hold Crash's pad buttons (pc/pad.c PAD_* bits) for a number of reads */
extern int crashcraft_pad_bits, crashcraft_pad_frames;
/* Outside every zone of the level (fell off it)? */
int crashcraft_game_outside_level(const float pos[3]);
/* Steve just landed: crates under him react as to Crash's jump if they're bounce crates. */
void crashcraft_game_landed(const float feet[3]);
/* Upward speed a bounce crate gave the player since the last call (blocks per tick, 0 none). */
float crashcraft_game_take_launch(void);
/* Steve rising: a crate right above his head breaks as when Crash hits it from below. */
void crashcraft_game_head_bump(const float feet[3], float head_height);
/* Enemies and boxes near a point. */
int crashcraft_game_actors(crashcraft_actor *out, int max, const float near[3], float radius);
/* Minecraft hit an object: Crash's spin (or a stomp from above). */
void crashcraft_game_hit_actor(uint32_t id, float damage, int from_above);
/* Minecraft's player died / fell into a hazard: Crash dies (his own death and respawn). */
void crashcraft_game_kill_crash(uint32_t event);
/* Damage Crash's world dealt to the player since the last call (hits; 0 if none). */
int crashcraft_game_take_damage(void);
/* Each frame, before objects update / after: hold Crash where Steve is. */
void crashcraft_game_puppet_before(void);
void crashcraft_game_puppet_after(void);
/* c1 skips drawing this object (Crash while Steve plays). */
int crashcraft_game_hide_object(const void *obj);
/* The frame's view: c1 render units -> clip (column-major), for Minecraft's meshes. */
int crashcraft_game_view_proj(float out[16]);
/* Window drawable size. */
void crashcraft_game_drawable_size(int *w, int *h);
/* One-line state for the log. */
void crashcraft_game_debug_state(char *buf, int len);

/* c1-side hooks (crashcraft_game.c); objects are gool_object* */
void crashcraft_game_camera_begin(void);
/* main.c: a finished level goes on to the next one (lid_t in, lid_t out) */
int crashcraft_game_next_level(int next);
void crashcraft_game_camera_end(void);
int crashcraft_game_skip_object(void *obj);
int crashcraft_game_event_to_crash(void *sender, uint32_t event);
/* GoolObjectBound: does this object get to touch Crash? (boxes don't while Steve plays) */
int crashcraft_game_touches_crash(void *obj);

#define CRASHCRAFT_TRI_SOLID  0x1
#define CRASHCRAFT_TRI_HAZARD 0x2   /* not collision */

#ifdef __cplusplus
}
#endif

#endif
