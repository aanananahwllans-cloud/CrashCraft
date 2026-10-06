// CrashCraft host: the per-frame glue between c1 and Minecraft (the role SkyCraft's Game.cpp plays for
// Skyrim; adapted from SM64Craft's host.cpp). Main thread only, apart from the collision worker.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <SDL2/SDL.h>

#include "collision.h"
#include "crashcraft.h"
#include "link.h"
#include "render.h"

namespace crashcraft
{
	namespace
	{
		constexpr double kUnits = proto::kUnitsPerBlock;  // c1 render units per block
		constexpr double kLevelSpacing = 8192.0;          // blocks between levels in Minecraft's world
		constexpr float  kPi = 3.14159265f;
		constexpr float  kDeg = kPi / 180.0f;
		constexpr float  kFps = 30.0f;                    // c1 runs Crash at 30 frames per second

		bool gEnabled = true;
		bool gStarted = false;

		proto::SkyState st{};
		proto::McState  mc{};
		bool            haveMc = false;
		bool            mcWasAlive = false;
		std::uint32_t   lastMcPid = 0;
		std::uint32_t   epoch = 0;
		std::uint32_t   teleportSeq = 0;
		bool            teleportPending = true;
		bool            collisionPending = false;
		int             settleFrames = 0;
		int             areaId = 0;
		double          offset[3] = { 0.0, 64.0, 0.0 };

		float  yaw = 0.0f, pitch = 0.0f;
		float  sensitivity = 0.5f;
		double lookDx = 0.0, lookDy = 0.0;

		bool  wasTakeover = false;
		bool  wasMcMode = false;
		bool  puppeting = false;
		int   width = 1024, height = 768;
		float cursorX = 0.0f, cursorY = 0.0f;
		int   frameCounter = 0;
		double lastFeet[3] = {};
		bool   wasOnGround = true;
		double fallPeakY = 0.0;
		float  lastGroundY = 0.0f;
		bool   tpOverride = false;      // test command "tp": teleport here instead of to Crash
		float  tpTarget[3] = {};
		int    hazardCooldown = 0;

		proto::WorldEntities gEntities;
		bool                 gHaveEntities = false;

		// ---- coordinates -------------------------------------------------------------------------

		void ToMc(const float a_c1[3], double a_out[3])
		{
			for (int i = 0; i < 3; ++i) {
				a_out[i] = a_c1[i] / kUnits + offset[i];
			}
		}

		void ToC1(double a_x, double a_y, double a_z, float a_out[3])
		{
			a_out[0] = float((a_x - offset[0]) * kUnits);
			a_out[1] = float((a_y - offset[1]) * kUnits);
			a_out[2] = float((a_z - offset[2]) * kUnits);
		}

		void LookDir(float a_yaw, float a_pitch, float a_out[3])
		{
			const float y = a_yaw * kDeg, p = a_pitch * kDeg;
			a_out[0] = -std::sin(y) * std::cos(p);
			a_out[1] = -std::sin(p);
			a_out[2] = std::cos(y) * std::cos(p);
		}

		bool ScreenOpen() { return haveMc && (mc.flags & proto::kMcScreenOpen); }
		bool InWorld() { return haveMc && (mc.flags & proto::kMcInWorld); }

		// ---- collision ---------------------------------------------------------------------------

		std::vector<McTri> ConvertTris(const std::vector<crashcraft_tri>& a_src)
		{
			std::vector<McTri> out;
			out.reserve(a_src.size());
			for (const auto& t : a_src) {
				if (!(t.flags & CRASHCRAFT_TRI_SOLID)) {
					continue;
				}
				McTri m;
				for (int v = 0; v < 3; ++v) {
					for (int i = 0; i < 3; ++i) {
						m.v[v * 3 + i] = float(t.v[v * 3 + i] / kUnits + offset[i]);
					}
				}
				m.flags = 0;
				out.push_back(m);
			}
			return out;
		}

		void RebuildCollision()
		{
			const int n = crashcraft_game_static_tris(nullptr, 0);
			std::vector<crashcraft_tri> tris(std::max(n, 0));
			if (n > 0) {
				crashcraft_game_static_tris(tris.data(), n);
			}
			// Fit the level into Minecraft's build height: its lowest floor near y = -48.
			float lo = 1e30f, hi = -1e30f;
			for (const auto& t : tris) {
				for (int v = 0; v < 3; ++v) {
					lo = std::min(lo, t.v[v * 3 + 1]);
					hi = std::max(hi, t.v[v * 3 + 1]);
				}
			}
			if (lo <= hi) {
				const double span = (hi - lo) / kUnits;
				offset[1] = span < 360.0 ? -48.0 - lo / kUnits : 128.0 - (lo + hi) * 0.5 / kUnits;
				Log("level collision: %d triangles (%d cells), height %.0f blocks, y offset %.1f", n, n / 12, span, offset[1]);
			}
			float crash[3];
			crashcraft_game_crash_pos(crash, nullptr);
			double near[3];
			ToMc(crash, near);
			Collision::Get().Reset(epoch, ConvertTris(tris), near);
			crashcraft_game_cache_level();
		}

		void UpdateDynamicCollision(const float a_near[3])
		{
			static std::vector<crashcraft_tri> tris(4096);
			const int n = crashcraft_game_dynamic_tris(tris.data(), int(tris.size()), a_near, 6000.0f);
			std::vector<crashcraft_tri> used(tris.begin(), tris.begin() + std::min<std::size_t>(std::max(n, 0), tris.size()));
			double near[3];
			ToMc(a_near, near);
			Collision::Get().UpdateDynamic(ConvertTris(used), near);
		}
	}
}

using namespace crashcraft;

extern "C" { static void TestCommands(); }

extern "C" {

void crashcraft_log(const char* a_fmt, ...)
{
	va_list args;
	va_start(args, a_fmt);
	LogV(a_fmt, args);
	va_end(args);
}

int crashcraft_enabled(void)
{
	return gEnabled ? 1 : 0;
}

void crashcraft_init(void)
{
	if (gStarted) {
		return;
	}
	gStarted = true;
	const char* off = SDL_getenv("CRASHCRAFT_OFF");
	if (off && *off && *off != '0') {
		gEnabled = false;
		Log("disabled by CRASHCRAFT_OFF");
		return;
	}
	if (!Link::Get().Create()) {
		gEnabled = false;
		return;
	}
	Collision::Get().Start();
	Log("CrashCraft ready: start the CrashCraft Minecraft instance (Minecraft 26.3 + Fabric)");
}

void crashcraft_frame_begin(void)
{
	if (!gEnabled) {
		return;
	}
	auto& link = Link::Get();
	link.Heartbeat();
	++frameCounter;
	TestCommands();
	crashcraft_game_drawable_size(&width, &height);

	// ---- Minecraft's side of the link --------------------------------------------------------
	const bool mcAlive = link.McAlive();
	haveMc = mcAlive && (link.ReadMcState(mc) || haveMc);
	const auto mcPid = link.McPid();
	const bool newMcProcess = mcAlive && mcPid != 0 && mcPid != lastMcPid;
	if (mcAlive) {
		lastMcPid = mcPid;
	}
	if (mcAlive && (!mcWasAlive || newMcProcess)) {
		Log("Minecraft connected (pid %u)", mcPid);
		link.ResetOverlay();
		++epoch;
		collisionPending = true;
		settleFrames = 2;
		teleportPending = true;
	}
	if (!mcAlive && mcWasAlive) {
		Log("Minecraft went away");
	}
	mcWasAlive = mcAlive;
	if (haveMc && mc.sensitivity > 0.0f) {
		sensitivity = mc.sensitivity;
	}

	// ---- which level we're in ----------------------------------------------------------------
	const int  id = crashcraft_game_area_id();
	const bool inLevel = id != 0;
	if (inLevel && id != areaId) {
		Log("level %d", id - 1);
		areaId = id;
		offset[0] = (id % 64) * kLevelSpacing;
		offset[2] = 0.0;
		++epoch;
		collisionPending = true;
		settleFrames = 10;
		teleportPending = true;
	}
	if (collisionPending && inLevel && mcAlive) {
		if (settleFrames > 0) {
			--settleFrames;
		} else {
			RebuildCollision();
			collisionPending = false;
		}
	}

	// ---- Crash acting on his own (death, respawn, warps, level end) --------------------------
	const bool takeover = inLevel && crashcraft_game_takeover();
	if (wasTakeover && !takeover) {
		teleportPending = true;  // Minecraft picks up wherever Crash came back
	}
	wasTakeover = takeover;

	float crashPos[3], crashYaw = 0.0f;
	crashcraft_game_crash_pos(crashPos, &crashYaw);
	double crashMc[3];
	ToMc(crashPos, crashMc);

	if (tpOverride) {
		// test command: put Crash there first, so Steve and Crash agree
		std::memcpy(crashPos, tpTarget, sizeof(crashPos));
		ToMc(crashPos, crashMc);
		g_crashcraft_puppet.pos[0] = tpTarget[0], g_crashcraft_puppet.pos[1] = tpTarget[1], g_crashcraft_puppet.pos[2] = tpTarget[2];
	}
	if (teleportPending && inLevel && !takeover && !collisionPending) {
		++teleportSeq;
		teleportPending = false;
		if (!tpOverride) {
			yaw = crashYaw;
			pitch = 0.0f;
		}
		tpOverride = false;
		Log("teleport %u to %.2f %.2f %.2f (yaw %.0f)", teleportSeq, crashMc[0], crashMc[1], crashMc[2], yaw);
	}

	{
		static std::string lastState;
		char               state[320];
		crashcraft_game_debug_state(state, sizeof(state));
		std::string now = std::string(state) + (ScreenOpen() ? ", mc screen" : "");
		if (now != lastState) {
			Log("state: %s", now.c_str());
			lastState = now;
		}
	}

	// ---- input mode and mouse look -----------------------------------------------------------
	const bool screenOpen = ScreenOpen();
	const bool crashOwns = crashcraft_game_input_owned() || takeover;
	const bool mcMode = InWorld() && inLevel && (!crashOwns || screenOpen);
	if (!mcMode && wasMcMode) {
		link.PushInput(proto::kInReleaseAll, 0);
	}
	wasMcMode = mcMode;
	{
		static bool lastRelative = false;
		const bool  relative = mcMode && !screenOpen && SDL_GetKeyboardFocus() != nullptr;
		if (relative != lastRelative) {
			SDL_SetRelativeMouseMode(relative ? SDL_TRUE : SDL_FALSE);
			lastRelative = relative;
			Log("controls: %s", relative ? "Minecraft (mouse look)" : "Crash / cursor");
		}
	}
	if (mcMode && !screenOpen) {
		// Minecraft's own formula, integrated here so the camera has no added latency.
		const float s = sensitivity * 0.6f + 0.2f;
		const float factor = s * s * s * 8.0f * 0.15f;
		yaw = std::fmod(yaw + float(lookDx) * factor, 360.0f);
		pitch = std::clamp(pitch + float(lookDy) * factor, -90.0f, 90.0f);
	}
	lookDx = lookDy = 0.0;

	// ---- Crash follows Steve -------------------------------------------------------------------
	const bool arrived = InWorld() && mc.teleportAck == teleportSeq;
	const bool puppet = arrived && inLevel && !takeover && !collisionPending && !(mc.flags & proto::kMcDead);
	if (puppet != puppeting) {
		Log("puppet %s", puppet ? "on (Minecraft drives Crash)" : "off");
	}
	puppeting = puppet;
	auto& pp = g_crashcraft_puppet;
	pp.active = puppet ? 1 : 0;
	pp.landed = 0;
	if (puppet) {
		ToC1(mc.x, mc.y, mc.z, pp.pos);
		const float tickMs = mc.tickMs > 0.0f ? mc.tickMs : 50.0f;
		const float perFrame = float(kUnits) * (1000.0f / kFps) / tickMs;
		pp.vel[0] = float(mc.curX - mc.prevX) * perFrame;
		pp.vel[1] = float(mc.curY - mc.prevY) * perFrame;
		pp.vel[2] = float(mc.curZ - mc.prevZ) * perFrame;
		pp.yaw_deg = yaw;
		const bool onGround = (mc.flags & proto::kMcOnGround) != 0;
		if (!onGround) {
			fallPeakY = wasOnGround ? mc.y : std::max(fallPeakY, mc.y);
		} else if (!wasOnGround) {
			pp.landed = 1;  // Steve came down on whatever is under him
			pp.fall_speed = int((fallPeakY - mc.y) * kUnits);
		}
		pp.on_ground = onGround ? 1 : 0;
		wasOnGround = onGround;
		if (onGround) {
			lastGroundY = pp.pos[1];
		}
		if (pp.landed) {
			crashcraft_game_landed(pp.pos);
		}
		if (!onGround && mc.curY > mc.prevY) {
			const float eye = mc.eyeY > mc.y ? float(mc.eyeY - mc.y) : 1.62f;
			crashcraft_game_head_bump(pp.pos, (eye + 0.18f) * float(kUnits));  // top of the head
		}
		if (const float up = crashcraft_game_take_launch(); up > 0.0f) {
			link.PushInput(proto::kInLaunch, 0, std::int32_t(up * 1000.0f));
		}
		lastFeet[0] = mc.x, lastFeet[1] = mc.y, lastFeet[2] = mc.z;

		// Crash's death volumes (pits, deep water, electricity) aren't Minecraft collision: walking
		// into one is Crash's death, then both come back at the checkpoint.
		if (hazardCooldown > 0) {
			--hazardCooldown;
		} else {
			std::uint32_t hazard = crashcraft_game_hazard_at(pp.pos);
			// Off the edge: outside every zone and well below where he last stood.
			if (!hazard && !onGround && pp.pos[1] < lastGroundY - 3.0f * float(kUnits) && crashcraft_game_outside_level(pp.pos)) {
				hazard = 0x900;  // GOOL_EVENT_FALL_KILL
				Log("Steve fell off the level");
			}
			if (hazard) {
				Log("Steve walked into a hazard (event %x) at %.0f %.0f %.0f (last ground %.0f): Crash's death", hazard, pp.pos[0], pp.pos[1], pp.pos[2], lastGroundY);
				hazardCooldown = 60;
				crashcraft_game_kill_crash(hazard);
				pp.active = 0;
				puppeting = false;
			}
		}
	}

	// ---- the camera: Minecraft's eye -----------------------------------------------------------
	auto& cam = g_crashcraft_camera;
	cam.active = (inLevel && InWorld() && puppeting) ? 1 : 0;
	if (cam.active) {
		double eye[3] = { mc.eyeX, mc.eyeY, mc.eyeZ };
		float  lookPitch = pitch, lookYaw = yaw;
		if (mc.cameraMode == 0 && mc.bobAmount != 0.0f) {
			// Minecraft's walk bob (GameRenderer.bobView) as a camera offset (SkyCraft's conversion).
			const float phase = mc.bobPhase * kPi, bob = mc.bobAmount;
			const float side = -std::sin(phase) * bob * 0.5f, lift = std::fabs(std::cos(phase) * bob);
			lookPitch += std::fabs(std::cos(phase - 0.2f) * bob) * 5.0f;
			const float h = yaw * kDeg;
			eye[0] += std::cos(h) * side;
			eye[2] += std::sin(h) * side;
			eye[1] += lift;
		}
		if (mc.cameraMode != 0 && mc.cameraDistance > 0.0f) {
			if (mc.cameraMode == 2) {
				lookYaw += 180.0f;
				lookPitch = -lookPitch;
			}
			float dir[3];
			LookDir(lookYaw, lookPitch, dir);
			for (int i = 0; i < 3; ++i) {
				eye[i] -= dir[i] * mc.cameraDistance;
			}
		}
		ToC1(eye[0], eye[1], eye[2], cam.pos);
		cam.yaw_deg = lookYaw;
		cam.pitch_deg = std::clamp(lookPitch, -89.9f, 89.9f);
		cam.fov_deg = mc.fovDeg > 1.0f ? mc.fovDeg : 70.0f;
	}

	// ---- what Minecraft needs from us this frame -----------------------------------------------
	st.flags = 0;
	if (inLevel) {
		st.flags |= proto::kSkyInGame;
	}
	if (crashOwns && inLevel) {
		st.flags |= proto::kSkyMenuOpen;
	}
	if (!inLevel || collisionPending) {
		st.flags |= proto::kSkyLoading;
	}
	st.worldId = std::uint32_t(areaId);
	st.collisionEpoch = epoch;
	st.posX = crashMc[0], st.posY = crashMc[1], st.posZ = crashMc[2];
	st.yaw = yaw;
	st.pitch = pitch;
	st.teleportSeq = teleportSeq;
	st.viewportW = std::uint32_t(std::min(width, int(proto::kMaxOverlayW)));
	st.viewportH = std::uint32_t(std::min(height, int(proto::kMaxOverlayH)));
	st.gameHour = 12.0f;
	link.WriteSkyState(st);

	// Minecraft's meshes, atlas and models (bounded per frame so a burst can't stall a frame).
	link.DrainRender([](std::uint32_t a_type, const std::uint8_t* a_payload, std::uint32_t a_bytes) { render::HandleMessage(a_type, a_payload, a_bytes); },
		24ull << 20);
	gHaveEntities = InWorld() && link.ReadWorldEntities(gEntities);

	// ---- Minecraft's events: hits, deaths, explosions ------------------------------------------
	proto::McEvent ev;
	while (link.PopEvent(ev)) {
		switch (ev.type) {
			case proto::kEvHitActor:
				crashcraft_game_hit_actor(ev.formId, ev.a, (ev.flags & proto::kHitCritical) != 0);
				break;
			case proto::kEvPlayerDied:
				Log("Minecraft's player died: Crash too");
				crashcraft_game_kill_crash(0);
				break;
			case proto::kEvExplosion: {
				// TNT and creepers: everything in the blast takes Crash's spin.
				float centre[3];
				ToC1(ev.a, ev.b, ev.c, centre);
				crashcraft_actor actors[64];
				const int n = crashcraft_game_actors(actors, 64, centre, float(ev.d * kUnits * 1.5));
				for (int i = 0; i < n; ++i) {
					crashcraft_game_hit_actor(actors[i].id, 20.0f, 0);
				}
				break;
			}
			default:
				break;
		}
	}

	if (const int hits = crashcraft_game_take_damage(); hits > 0 && puppeting) {
		// One hit from Crash's world = 4 hearts (Crash dies in one hit; Steve gets a few).
		link.PushInput(proto::kInHurt, proto::kHurtMelee, hits * 40 * 100, 0, 0);
		Log("Crash's world hurt the player: %d hit(s)", hits);
	}

	// Enemies and boxes near the player, mirrored in Minecraft as hittable stand-ins.
	if (inLevel && InWorld()) {
		crashcraft_actor actors[proto::kMaxActors];
		const int        n = crashcraft_game_actors(actors, int(proto::kMaxActors), crashPos, 6400.0f);
		proto::ActorRecord records[proto::kMaxActors];
		for (int i = 0; i < n; ++i) {
			auto&       r = records[i];
			const auto& a = actors[i];
			std::memset(&r, 0, sizeof(r));
			r.formId = a.id;
			r.flags = a.hostile ? proto::kActorHostile : 0;
			double p[3];
			ToMc(a.pos, p);
			// stand-ins a little bigger than the real thing: Minecraft's aim hits them first (crates
			// sit inside their own solid block), and enemies are easier to hit
			const float pad = a.kind == CRASHCRAFT_ACTOR_BOX ? 0.15f : 0.3f;
			r.x = float(p[0]), r.y = float(p[1]) - pad, r.z = float(p[2]);
			r.yaw = a.yaw_deg;
			r.width = std::max(0.3f, float(std::max(a.half[0], a.half[2]) * 2.0 / kUnits)) + 2.0f * pad;
			r.height = std::max(0.3f, float(a.half[1] * 2.0 / kUnits)) + 2.0f * pad;
			r.healthFrac = 1.0f;
			r.level = 1;
			std::memcpy(r.name, a.name, sizeof(r.name));
			r.name[sizeof(r.name) - 1] = '\0';
		}
		link.WriteActors(records, std::uint32_t(n));
		if (!collisionPending && (frameCounter % 3) == 0 && mcAlive) {
			UpdateDynamicCollision(crashPos);
		}
	} else {
		link.WriteActors(nullptr, 0);
	}
}

void crashcraft_render_world(void)
{
	if (!gEnabled || !g_crashcraft_camera.active) {
		return;
	}
	render::View view{};
	if (!crashcraft_game_view_proj(view.viewProj)) {
		return;
	}
	view.xAdjust = 1.0f;
	std::memcpy(view.offset, offset, sizeof(offset));
	view.width = width;
	view.height = height;
	view.feet[0] = mc.x, view.feet[1] = mc.y, view.feet[2] = mc.z;
	view.showAvatar = puppeting && mc.cameraMode != 0;
	view.daylight = 1.0f;
	render::DrawWorld(view, gHaveEntities ? &gEntities : nullptr);
}

// Test commands: write lines to "crashcraft.cmd" next to the exe; read (and deleted) once a frame.
//   key <scancode> <1|0>    click <button>    look <yaw> <pitch>    tp <x> <y> <z> (c1 units)    shot
static void TestCommands()
{
	FILE* f = std::fopen("crashcraft.cmd", "rb");
	if (!f) {
		return;
	}
	char line[256];
	auto& link = Link::Get();
	while (std::fgets(line, sizeof(line), f)) {
		int   a = 0, b = 0;
		float x = 0, y = 0, z = 0;
		if (std::sscanf(line, "key %d %d", &a, &b) == 2) {
			link.PushInput(proto::kInKey, std::uint16_t(a), b);
		} else if (std::sscanf(line, "click %d", &a) == 1) {
			link.PushInput(proto::kInMouseButton, std::uint16_t(a), 1);
			link.PushInput(proto::kInMouseButton, std::uint16_t(a), 0);
		} else if (std::sscanf(line, "look %f %f", &x, &y) == 2) {
			yaw = x, pitch = y;
		} else if (std::sscanf(line, "tp %f %f %f", &x, &y, &z) == 3) {
			tpTarget[0] = x, tpTarget[1] = y, tpTarget[2] = z;
			tpOverride = true;
			teleportPending = true;
			crashcraft_game_debug_jump_zone(tpTarget);
		} else if (char name[32]; std::sscanf(line, "event %31s %x", name, &a) == 2) {
			crashcraft_game_debug_event(name, std::uint32_t(a));
		} else if (std::sscanf(line, "pad %x %d", &a, &b) == 2) {
			crashcraft_pad_bits = a;
			crashcraft_pad_frames = b;
		} else if (std::sscanf(line, "cell %f %f %f", &x, &y, &z) == 3) {
			const float p[3] = { x, y, z };
			crashcraft_game_debug_point(p);
		} else if (std::strncmp(line, "kill", 4) == 0) {
			crashcraft_game_kill_crash(0);
		} else if (std::strncmp(line, "shot", 4) == 0) {
			if (FILE* r = std::fopen("shot.req", "wb")) {
				std::fclose(r);
			}
		} else {
			continue;
		}
		Log("test command: %s", line);
	}
	std::fclose(f);
	std::remove("crashcraft.cmd");
}

static void DebugShot()
{
	// Debug: create "shot.req" next to the exe and the next frame is written to "shot.png".
	if ((frameCounter % 15) != 0) {
		return;
	}
	if (FILE* f = std::fopen("shot.req", "rb")) {
		std::fclose(f);
		std::remove("shot.req");
		const bool ok = render::SavePng(width, height, "shot.png");
		int        sections = 0;
		long long  verts = 0;
		bool       atlas = false;
		render::Stats(sections, verts, atlas);
		Log("screenshot %s (%dx%d); mc %s, inWorld %d, puppet %d, level %d, tp %u/%u, pos %.2f %.2f %.2f, look %.1f %.1f, pending collision %zu, "
			"mc sections %d (%lld vertices), atlas %d",
			ok ? "saved" : "FAILED", width, height, haveMc ? "linked" : "not linked", InWorld() ? 1 : 0, puppeting ? 1 : 0, areaId - 1, mc.teleportAck,
			teleportSeq, mc.x, mc.y, mc.z, yaw, pitch, Collision::Get().Pending(), sections, verts, atlas ? 1 : 0);
	}
}

void crashcraft_render_overlay(void)
{
	if (gEnabled && InWorld() && crashcraft_game_in_level()) {
		render::OverlayParams p{};
		p.width = width;
		p.height = height;
		p.guiScale = int(mc.guiScale);
		p.crosshair = puppeting && mc.cameraMode == 0 && !ScreenOpen() && !crashcraft_game_input_owned();
		p.hideCrosshair = false;
		p.cursorOn = ScreenOpen();
		p.cursorX = cursorX;
		p.cursorY = cursorY;
		render::DrawOverlay(Link::Get(), p);
	}
	DebugShot();  // after the overlay: the frame exactly as the player sees it
}

int crashcraft_handle_sdl_event(const void* a_event)
{
	if (!gEnabled || !InWorld() || !crashcraft_game_in_level()) {
		return 0;
	}
	const auto* e = static_cast<const SDL_Event*>(a_event);
	auto&       link = Link::Get();
	const bool  screenOpen = ScreenOpen();
	const bool  crashOwns = crashcraft_game_input_owned() || crashcraft_game_takeover();
	const bool  toMc = !crashOwns || screenOpen;
	if (!toMc) {
		return 0;  // Crash's menus and his own moments: c1's keyboard/mouse mapping (pc/pad.c)
	}

	switch (e->type) {
		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			const bool down = e->type == SDL_KEYDOWN;
			const int  sc = e->key.keysym.scancode;
			if (sc == SDL_SCANCODE_F11 || sc == SDL_SCANCODE_F12) {
				return 0;
			}
			if (!screenOpen) {
				// The few keys that belong to Crash while playing.
				if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_ESCAPE) {
					if (down && !e->key.repeat) {
						g_crashcraft_puppet.press_start = 2;  // Crash's pause menu
					}
					return 1;
				}
				if (sc == SDL_SCANCODE_O) {
					if (down && !e->key.repeat) {
						link.PushInput(proto::kInOpenMenu, 0);  // Minecraft's pause / options menu
					}
					return 1;
				}
			}
			link.PushInput(proto::kInKey, std::uint16_t(sc), down ? 1 : 0);
			return 1;
		}
		case SDL_TEXTINPUT: {
			if (!screenOpen) {
				return 1;
			}
			const auto* s = reinterpret_cast<const unsigned char*>(e->text.text);
			while (*s) {
				std::uint32_t cp = *s++;
				int           extra = 0;
				if (cp >= 0xF0) {
					cp &= 0x07, extra = 3;
				} else if (cp >= 0xE0) {
					cp &= 0x0F, extra = 2;
				} else if (cp >= 0xC0) {
					cp &= 0x1F, extra = 1;
				}
				while (extra-- > 0 && *s) {
					cp = (cp << 6) | (*s++ & 0x3F);
				}
				link.PushInput(proto::kInText, 0, std::int32_t(cp));
			}
			return 1;
		}
		case SDL_MOUSEMOTION: {
			if (screenOpen) {
				int ww = 0, wh = 0;
				SDL_GetWindowSize(SDL_GetWindowFromID(e->motion.windowID), &ww, &wh);
				const float sx = ww > 0 ? float(width) / ww : 1.0f, sy = wh > 0 ? float(height) / wh : 1.0f;
				cursorX = e->motion.x * sx;
				cursorY = e->motion.y * sy;
				link.PushInput(proto::kInCursor, 0, std::int32_t(cursorX), std::int32_t(cursorY));
			} else {
				lookDx += e->motion.xrel;
				lookDy += e->motion.yrel;
			}
			return 1;
		}
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			link.PushInput(proto::kInMouseButton, std::uint16_t(e->button.button), e->type == SDL_MOUSEBUTTONDOWN ? 1 : 0);
			return 1;
		case SDL_MOUSEWHEEL:
			link.PushInput(proto::kInScroll, 0, std::int32_t(e->wheel.y * 120));
			return 1;
		case SDL_WINDOWEVENT:
			if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
				link.PushInput(proto::kInReleaseAll, 0);
			}
			return 0;
		default:
			return 0;
	}
}

}  // extern "C"
