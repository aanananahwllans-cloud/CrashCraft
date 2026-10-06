// Draws what Minecraft ships through the render ring (block meshes, the player model, entities,
// particles, item sprites, the block outline) inside c1's frame, depth-tested against the depth
// buffer c1 now writes, and composites Minecraft's hand/HUD overlay on top. OpenGL, main thread only.
#pragma once

#include <cstdint>

#include "link.h"

namespace crashcraft::render
{
	struct View
	{
		float  viewProj[16];   // c1 render units -> clip, column-major (GL's own layout)
		float  xAdjust;        // extra clip x scale (1: none)
		double offset[3];      // Minecraft coords of c1's origin (mc = units / kUnitsPerBlock + offset)
		int    width, height;  // drawable size
		double feet[3];        // Minecraft player feet (for the avatar)
		bool   showAvatar;
		float  daylight;       // 0..1
	};

	void HandleMessage(std::uint32_t a_type, const std::uint8_t* a_payload, std::uint32_t a_bytes);
	void ClearAll();
	void DrawWorld(const View& a_view, const proto::WorldEntities* a_entities);

	struct OverlayParams
	{
		int   width, height;
		bool  crosshair;      // Minecraft's crosshair is up (invert-blend it)
		bool  hideCrosshair;  // ...but leave it out (the portal gun draws Portal 2's)
		int   guiScale;
		bool  cursorOn;       // a Minecraft screen is open: draw the mouse cursor
		float cursorX, cursorY;
	};
	void DrawOverlay(Link& a_link, const OverlayParams& a_params);
	// Debug: the frame as a PNG (back buffer, before it is shown).
	bool SavePng(int a_width, int a_height, const char* a_path);
	void Stats(int& a_sections, long long& a_vertices, bool& a_atlas);
}
