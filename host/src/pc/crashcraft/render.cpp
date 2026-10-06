#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <cstdio>

#include "pc/gfx/glload.h"
#include "png.h"

namespace crashcraft::render
{
	namespace
	{
		constexpr double kUnits = proto::kUnitsPerBlock;

		// Our own vertex: 28 bytes.
		struct GVert
		{
			float        x, y, z;
			float        u, v;
			std::uint8_t r, g, b, a;
			std::uint8_t blockLight, skyLight, shade, flags;
		};
		static_assert(sizeof(GVert) == 28);

		struct Mesh
		{
			GLuint  vbo{ 0 };
			GLsizei opaque{ 0 };       // first `opaque` vertices: solid + cutout
			GLsizei translucent{ 0 };  // then these: blended
		};

		struct Batch
		{
			std::uint32_t texture;
			GLint         first;
			GLsizei       count;
			bool          translucent;
		};

		struct Dynamic
		{
			GLuint             vbo{ 0 };
			std::vector<Batch> batches;
			bool               valid{ false };
		};

		bool   gInit = false, gBroken = false;
		GLuint gProgram = 0, gVao = 0;
		GLint  uMvp = -1, uOrigin = -1, uScale = -1, uXAdjust = -1, uTex = -1, uAlphaTest = -1, uDay = -1, uLit = -1, uTint = -1;
		GLint  aPos = -1, aUV = -1, aColor = -1, aLight = -1;

		GLuint gOverlayProgram = 0, gOverlayTex = 0, gQuadVbo = 0;
		GLint  oTex = -1, oFlip = -1, oCursor = -1, oCursorOn = -1, oViewport = -1, oInvert = -1, oMode = -1, oPos = -1;
		int    gOverlayW = 0, gOverlayH = 0;
		bool   gOverlayBottomUp = true, gHaveOverlay = false;

		GLuint gAtlas = 0;
		int    gAtlasW = 0, gAtlasH = 0;
		std::unordered_map<std::uint32_t, GLuint>       gTextures;
		std::unordered_map<std::uint64_t, Mesh>         gSections;
		std::unordered_map<std::uint64_t, std::int32_t> gSectionPos;  // key -> packed for origin
		Dynamic                                         gAvatar, gScene;
		double                                          gSceneOrigin[3]{};
		GLuint                                          gSpriteVbo = 0;

		std::uint64_t SectionKey(std::int32_t sx, std::int32_t sy, std::int32_t sz)
		{
			return (std::uint64_t(std::uint32_t(sx) & 0x3FFFFF) << 42) | (std::uint64_t(std::uint32_t(sy) & 0xFFFFF) << 22) | (std::uint64_t(std::uint32_t(sz) & 0x3FFFFF));
		}
		void UnpackSection(std::uint64_t k, int& sx, int& sy, int& sz)
		{
			auto sext = [](std::uint64_t v, int bits) {
				const std::int64_t s = static_cast<std::int64_t>(v);
				return static_cast<int>((s << (64 - bits)) >> (64 - bits));
			};
			sx = sext((k >> 42) & 0x3FFFFF, 22);
			sy = sext((k >> 22) & 0xFFFFF, 20);
			sz = sext(k & 0x3FFFFF, 22);
		}

		const char* kVs = R"(#version 120
attribute vec3 aPos;
attribute vec2 aUV;
attribute vec4 aColor;
attribute vec4 aLight;
uniform mat4 uMvp;
uniform vec3 uOrigin;
uniform float uScale;
uniform float uXAdjust;
varying vec2 vUV;
varying vec4 vColor;
varying vec3 vLight;
void main() {
	vec4 c = uMvp * vec4(uOrigin + aPos * uScale, 1.0);
	c.x *= uXAdjust;
	gl_Position = c;
	vUV = aUV;
	vColor = aColor;
	vLight = vec3(aLight.x * 255.0 / 15.0, aLight.y * 255.0 / 15.0, aLight.z);
}
)";
		const char* kFs = R"(#version 120
uniform sampler2D uTex;
uniform float uAlphaTest;
uniform float uDay;
uniform float uLit;
uniform vec4 uTint;
varying vec2 vUV;
varying vec4 vColor;
varying vec3 vLight;
float curve(float f) { f = clamp(f, 0.0, 1.0); return f / (4.0 - 3.0 * f); }
void main() {
	vec4 c = texture2D(uTex, vUV) * vColor * uTint;
	if (c.a < uAlphaTest) discard;
	if (uLit > 0.5) {
		float sky = mix(0.16, 1.0, curve(vLight.y * uDay));
		float blk = curve(vLight.x);
		vec3 light = max(vec3(sky), vec3(1.0, 0.86, 0.68) * mix(0.16, 1.15, blk));
		c.rgb *= light * vLight.z;
	}
	gl_FragColor = c;
}
)";
		const char* kOverlayVs = R"(#version 120
attribute vec2 aPos;
varying vec2 vUV;
void main() { vUV = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }
)";
		// uMode 0: everything except the crosshair box (premultiplied blend); 1: only the crosshair
		// box, drawn with Minecraft's invert blend.
		const char* kOverlayFs = R"(#version 120
uniform sampler2D uTex;
uniform float uFlip;
uniform vec2 uCursor;
uniform float uCursorOn;
uniform vec2 uViewport;
uniform vec4 uInvert;
uniform float uMode;
varying vec2 vUV;
void main() {
	vec2 uv = vUV;
	if (uFlip > 0.5) uv.y = 1.0 - uv.y;
	vec2 p = vec2(gl_FragCoord.x, uViewport.y - gl_FragCoord.y);
	bool inInvert = all(greaterThanEqual(p, uInvert.xy)) && all(lessThan(p, uInvert.zw));
	vec4 c = texture2D(uTex, uv);
	if (uMode > 0.5) {
		if (!inInvert) discard;
		gl_FragColor = vec4(c.rgb, 0.0);
		return;
	}
	if (inInvert) c = vec4(0.0);
	if (uCursorOn > 0.5) {
		vec2 q = p - uCursor;
		if (q.x >= 0.0 && q.y >= 0.0 && q.y < 18.0 && q.x <= q.y * 0.6) {
			bool edge = q.x < 1.5 || q.x > q.y * 0.6 - 1.5 || q.y > 16.5;
			c = vec4(edge ? vec3(0.0) : vec3(1.0), 1.0);
		}
	}
	gl_FragColor = c;
}
)";

		GLuint Compile(GLenum a_type, const char* a_src)
		{
			GLuint s = glCreateShader(a_type);
			glShaderSource(s, 1, &a_src, nullptr);
			glCompileShader(s);
			GLint ok = 0;
			glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
			if (!ok) {
				char log[1024];
				glGetShaderInfoLog(s, sizeof(log), nullptr, log);
				Log("shader compile failed: %s", log);
			}
			return s;
		}

		GLuint Link2(const char* a_vs, const char* a_fs)
		{
			GLuint p = glCreateProgram();
			GLuint vs = Compile(GL_VERTEX_SHADER, a_vs), fs = Compile(GL_FRAGMENT_SHADER, a_fs);
			glAttachShader(p, vs);
			glAttachShader(p, fs);
			glLinkProgram(p);
			GLint ok = 0;
			glGetProgramiv(p, GL_LINK_STATUS, &ok);
			if (!ok) {
				char log[1024];
				glGetProgramInfoLog(p, sizeof(log), nullptr, log);
				Log("shader link failed: %s", log);
				return 0;
			}
			return p;
		}

		bool Init()
		{
			if (gInit) {
				return !gBroken;
			}
			gInit = true;
			if (!glGenVertexArrays || !glCreateProgram) {
				Log("OpenGL 3.0+ is needed for CrashCraft's renderer; Minecraft's world won't be drawn");
				gBroken = true;
				return false;
			}
			gProgram = Link2(kVs, kFs);
			gOverlayProgram = Link2(kOverlayVs, kOverlayFs);
			if (!gProgram || !gOverlayProgram) {
				gBroken = true;
				return false;
			}
			uMvp = glGetUniformLocation(gProgram, "uMvp");
			uOrigin = glGetUniformLocation(gProgram, "uOrigin");
			uScale = glGetUniformLocation(gProgram, "uScale");
			uXAdjust = glGetUniformLocation(gProgram, "uXAdjust");
			uTex = glGetUniformLocation(gProgram, "uTex");
			uAlphaTest = glGetUniformLocation(gProgram, "uAlphaTest");
			uDay = glGetUniformLocation(gProgram, "uDay");
			uLit = glGetUniformLocation(gProgram, "uLit");
			uTint = glGetUniformLocation(gProgram, "uTint");
			aPos = glGetAttribLocation(gProgram, "aPos");
			aUV = glGetAttribLocation(gProgram, "aUV");
			aColor = glGetAttribLocation(gProgram, "aColor");
			aLight = glGetAttribLocation(gProgram, "aLight");
			oTex = glGetUniformLocation(gOverlayProgram, "uTex");
			oFlip = glGetUniformLocation(gOverlayProgram, "uFlip");
			oCursor = glGetUniformLocation(gOverlayProgram, "uCursor");
			oCursorOn = glGetUniformLocation(gOverlayProgram, "uCursorOn");
			oViewport = glGetUniformLocation(gOverlayProgram, "uViewport");
			oInvert = glGetUniformLocation(gOverlayProgram, "uInvert");
			oMode = glGetUniformLocation(gOverlayProgram, "uMode");
			oPos = glGetAttribLocation(gOverlayProgram, "aPos");

			glGenVertexArrays(1, &gVao);
			glGenBuffers(1, &gQuadVbo);
			glGenBuffers(1, &gSpriteVbo);
			glGenBuffers(1, &gAvatar.vbo);
			glGenBuffers(1, &gScene.vbo);
			GLint prevBuf = 0;
			glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
			const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
			glBindBuffer(GL_ARRAY_BUFFER, gQuadVbo);
			glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
			glBindBuffer(GL_ARRAY_BUFFER, prevBuf);
			Log("renderer ready (%s)", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
			return true;
		}

		// Everything we touch, put back exactly as c1's renderer left it.
		struct SavedState
		{
			GLint  program, vao, arrayBuffer, activeTex, tex0, tex1, viewport[4], scissor[4], depthFunc, blendSrc, blendDst, blendSrcA, blendDstA;
			GLboolean depthTest, depthMask, blend, scissorTest, cull, polyOffset, colorMask[4];

			void Save()
			{
				glGetIntegerv(GL_CURRENT_PROGRAM, &program);
				glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
				glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
				glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTex);
				glActiveTexture(GL_TEXTURE1);
				glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex1);
				glActiveTexture(GL_TEXTURE0);
				glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
				glGetIntegerv(GL_VIEWPORT, viewport);
				glGetIntegerv(GL_SCISSOR_BOX, scissor);
				glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
				glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrc);
				glGetIntegerv(GL_BLEND_DST_RGB, &blendDst);
				glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcA);
				glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstA);
				depthTest = glIsEnabled(GL_DEPTH_TEST);
				glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
				blend = glIsEnabled(GL_BLEND);
				scissorTest = glIsEnabled(GL_SCISSOR_TEST);
				cull = glIsEnabled(GL_CULL_FACE);
				polyOffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
				glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
			}

			void Restore() const
			{
				glUseProgram(program);
				glBindVertexArray(vao);
				glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer);
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, tex1);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, tex0);
				glActiveTexture(activeTex);
				glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
				glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
				glDepthFunc(depthFunc);
				glBlendFuncSeparate(blendSrc, blendDst, blendSrcA, blendDstA);
				depthTest ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
				glDepthMask(depthMask);
				blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
				scissorTest ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
				cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
				polyOffset ? glEnable(GL_POLYGON_OFFSET_FILL) : glDisable(GL_POLYGON_OFFSET_FILL);
				glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
			}
		};

		GLuint NewTexture(int a_w, int a_h, const std::uint8_t* a_pixels)
		{
			GLuint t = 0;
			glGenTextures(1, &t);
			glBindTexture(GL_TEXTURE_2D, t);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, a_w, a_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, a_pixels);
			return t;
		}

		std::uint8_t FaceShade(std::uint32_t a_flags)
		{
			// Minecraft's fixed face shading (the export leaves it out): down, up, north, south, west, east.
			static constexpr std::uint8_t kShade[7] = { 255, 128, 255, 204, 204, 153, 153 };
			const std::uint32_t dir = (a_flags >> 4) & 7;
			return dir < 7 ? kShade[dir] : 255;
		}

		GVert Convert(const proto::RenVertex& a_v)
		{
			GVert g;
			g.x = a_v.x, g.y = a_v.y, g.z = a_v.z;
			g.u = a_v.u, g.v = a_v.v;
			g.r = std::uint8_t(a_v.color & 0xFF);
			g.g = std::uint8_t((a_v.color >> 8) & 0xFF);
			g.b = std::uint8_t((a_v.color >> 16) & 0xFF);
			g.a = std::uint8_t((a_v.color >> 24) & 0xFF);
			g.blockLight = std::uint8_t(a_v.light & 0xFF);
			g.skyLight = std::uint8_t((a_v.light >> 8) & 0xFF);
			g.shade = FaceShade(a_v.flags);
			g.flags = std::uint8_t(a_v.flags & 3);
			return g;
		}

		void UploadSection(const proto::RenSection& a_hdr, const proto::RenVertex* a_verts)
		{
			const auto key = SectionKey(a_hdr.sx, a_hdr.sy, a_hdr.sz);
			if (a_hdr.vertexCount == 0) {
				if (auto it = gSections.find(key); it != gSections.end()) {
					glDeleteBuffers(1, &it->second.vbo);
					gSections.erase(it);
				}
				return;
			}
			std::vector<GVert> solid, blended;
			for (std::uint32_t t = 0; t + 2 < a_hdr.vertexCount; t += 3) {
				auto& dst = (a_verts[t].flags & 2) ? blended : solid;
				for (int k = 0; k < 3; ++k) {
					dst.push_back(Convert(a_verts[t + k]));
				}
			}
			auto& mesh = gSections[key];
			if (!mesh.vbo) {
				glGenBuffers(1, &mesh.vbo);
			}
			mesh.opaque = static_cast<GLsizei>(solid.size());
			mesh.translucent = static_cast<GLsizei>(blended.size());
			solid.insert(solid.end(), blended.begin(), blended.end());
			glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
			glBufferData(GL_ARRAY_BUFFER, solid.size() * sizeof(GVert), solid.data(), GL_STATIC_DRAW);
		}

		void UploadDynamic(Dynamic& a_dst, const proto::RenBatch* a_batches, std::uint32_t a_batchCount, const proto::RenVertex* a_verts, std::uint32_t a_vertCount)
		{
			a_dst.batches.clear();
			a_dst.valid = a_batchCount > 0 && a_vertCount > 0;
			if (!a_dst.valid) {
				return;
			}
			std::vector<GVert> verts(a_vertCount);
			for (std::uint32_t i = 0; i < a_vertCount; ++i) {
				verts[i] = Convert(a_verts[i]);
			}
			for (std::uint32_t i = 0; i < a_batchCount; ++i) {
				const auto& b = a_batches[i];
				if (b.first + b.count > a_vertCount) {
					continue;
				}
				a_dst.batches.push_back({ b.texture, GLint(b.first), GLsizei(b.count), (b.flags & 1) != 0 });
			}
			glBindBuffer(GL_ARRAY_BUFFER, a_dst.vbo);
			glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(GVert), verts.data(), GL_STREAM_DRAW);
		}

		void BindVertexLayout()
		{
			const auto stride = sizeof(GVert);
			glEnableVertexAttribArray(aPos);
			glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(GVert, x)));
			glEnableVertexAttribArray(aUV);
			glVertexAttribPointer(aUV, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(GVert, u)));
			glEnableVertexAttribArray(aColor);
			glVertexAttribPointer(aColor, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void*>(offsetof(GVert, r)));
			glEnableVertexAttribArray(aLight);
			glVertexAttribPointer(aLight, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void*>(offsetof(GVert, blockLight)));
		}

		void SetOrigin(const View& a_view, double a_x, double a_y, double a_z)
		{
			glUniform3f(uOrigin, float((a_x - a_view.offset[0]) * kUnits), float((a_y - a_view.offset[1]) * kUnits), float((a_z - a_view.offset[2]) * kUnits));
		}

		GLuint TextureFor(std::uint32_t a_id)
		{
			if (a_id == 0) {
				return gAtlas;
			}
			auto it = gTextures.find(a_id);
			return it != gTextures.end() ? it->second : gAtlas;
		}

		void DrawDynamic(const View& a_view, const Dynamic& a_dyn, const double a_origin[3], bool a_translucent)
		{
			if (!a_dyn.valid || a_dyn.batches.empty()) {
				return;
			}
			glBindBuffer(GL_ARRAY_BUFFER, a_dyn.vbo);
			BindVertexLayout();
			SetOrigin(a_view, a_origin[0], a_origin[1], a_origin[2]);
			for (const auto& b : a_dyn.batches) {
				if (b.translucent != a_translucent) {
					continue;
				}
				glBindTexture(GL_TEXTURE_2D, TextureFor(b.texture));
				glDrawArrays(GL_TRIANGLES, b.first, b.count);
			}
		}

		// Item sprites, arrows, spinning dropped blocks: built on the CPU each frame in Minecraft coords
		// relative to the camera-independent origin (0, 0, 0) of the offset.
		void DrawEntities(const View& a_view, const proto::WorldEntities& a_we, const float a_camRight[3])
		{
			std::vector<GVert> verts;
			auto push = [&](float x, float y, float z, float u, float v) {
				GVert g{};
				g.x = x, g.y = y, g.z = z, g.u = u, g.v = v;
				g.r = g.g = g.b = g.a = 255;
				g.blockLight = 0, g.skyLight = 15, g.shade = 255, g.flags = 1;
				verts.push_back(g);
			};
			auto quad = [&](const float c[3], const float right[3], const float up[3], const float* uv) {
				const float p[4][3] = {
					{ c[0] - right[0] - up[0], c[1] - right[1] - up[1], c[2] - right[2] - up[2] },
					{ c[0] + right[0] - up[0], c[1] + right[1] - up[1], c[2] + right[2] - up[2] },
					{ c[0] + right[0] + up[0], c[1] + right[1] + up[1], c[2] + right[2] + up[2] },
					{ c[0] - right[0] + up[0], c[1] - right[1] + up[1], c[2] - right[2] + up[2] },
				};
				push(p[0][0], p[0][1], p[0][2], uv[0], uv[3]);
				push(p[1][0], p[1][1], p[1][2], uv[2], uv[3]);
				push(p[2][0], p[2][1], p[2][2], uv[2], uv[1]);
				push(p[0][0], p[0][1], p[0][2], uv[0], uv[3]);
				push(p[2][0], p[2][1], p[2][2], uv[2], uv[1]);
				push(p[3][0], p[3][1], p[3][2], uv[0], uv[1]);
			};
			const double ox = a_view.feet[0], oy = a_view.feet[1], oz = a_view.feet[2];  // local origin: precision
			for (std::uint32_t i = 0; i < a_we.count; ++i) {
				const auto& e = a_we.entities[i];
				const float c[3] = { float(e.x - ox), float(e.y - oy), float(e.z - oz) };
				const float s = e.scale > 0 ? e.scale : 0.5f;
				if (e.kind == proto::kWeItem || e.kind == proto::kWeArrow || e.kind == proto::kWeTrident) {
					// Turning about the vertical (items) or facing the camera (arrows, simplified).
					float right[3], up[3] = { 0, s * 0.5f, 0 };
					if (e.kind == proto::kWeItem) {
						const float yaw = e.yaw * 0.0174533f;
						right[0] = std::cos(yaw) * s * 0.5f, right[1] = 0, right[2] = std::sin(yaw) * s * 0.5f;
					} else {
						right[0] = a_camRight[0] * s * 0.5f, right[1] = 0, right[2] = a_camRight[2] * s * 0.5f;
					}
					const float cc[3] = { c[0], c[1] + up[1], c[2] };
					quad(cc, right, up, e.uv[0]);
				} else if (e.kind == proto::kWeBlock) {
					const float h = s * 0.5f;
					const float yaw = e.yaw * 0.0174533f;
					const float cx = std::cos(yaw) * h, sx = std::sin(yaw) * h;
					const float right[3] = { cx, 0, sx }, fwd[3] = { -sx, 0, cx }, up[3] = { 0, h, 0 };
					const float mid[3] = { c[0], c[1] + h, c[2] };
					// four sides, top, bottom
					for (int f = 0; f < 4; ++f) {
						const float sgn = (f & 1) ? -1.0f : 1.0f;
						const float* axis = (f < 2) ? fwd : right;
						const float* side = (f < 2) ? right : fwd;
						const float fc[3] = { mid[0] + axis[0] * sgn, mid[1], mid[2] + axis[2] * sgn };
						const float sd[3] = { side[0] * sgn, 0, side[2] * sgn };
						quad(fc, sd, up, e.uv[0]);
					}
					const float top[3] = { mid[0], mid[1] + h, mid[2] }, bot[3] = { mid[0], mid[1] - h, mid[2] };
					quad(top, right, fwd, e.uv[1]);
					quad(bot, right, fwd, e.uv[2]);
				}
			}
			if (verts.empty()) {
				return;
			}
			glBindBuffer(GL_ARRAY_BUFFER, gSpriteVbo);
			glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(GVert), verts.data(), GL_STREAM_DRAW);
			BindVertexLayout();
			SetOrigin(a_view, ox, oy, oz);
			glBindTexture(GL_TEXTURE_2D, gAtlas);
			glDrawArrays(GL_TRIANGLES, 0, GLsizei(verts.size()));
		}

		void DrawSelection(const View& a_view, const proto::WorldEntities& a_we)
		{
			if (!a_we.hasSelection) {
				return;
			}
			const double ox = a_we.selMin[0], oy = a_we.selMin[1], oz = a_we.selMin[2];
			const float  e = 0.002f;
			const float  lo[3] = { -e, -e, -e };
			const float  hi[3] = { float(a_we.selMax[0] - ox) + e, float(a_we.selMax[1] - oy) + e, float(a_we.selMax[2] - oz) + e };
			std::vector<GVert> v;
			auto p = [&](float x, float y, float z) {
				GVert g{};
				g.x = x, g.y = y, g.z = z;
				g.a = 102;  // Minecraft's outline: black at 40%
				g.shade = 255;
				v.push_back(g);
			};
			const float c[8][3] = { { lo[0], lo[1], lo[2] }, { hi[0], lo[1], lo[2] }, { hi[0], lo[1], hi[2] }, { lo[0], lo[1], hi[2] },
				{ lo[0], hi[1], lo[2] }, { hi[0], hi[1], lo[2] }, { hi[0], hi[1], hi[2] }, { lo[0], hi[1], hi[2] } };
			static constexpr int kEdges[12][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
			for (auto& edge : kEdges) {
				p(c[edge[0]][0], c[edge[0]][1], c[edge[0]][2]);
				p(c[edge[1]][0], c[edge[1]][1], c[edge[1]][2]);
			}
			glBindBuffer(GL_ARRAY_BUFFER, gSpriteVbo);
			glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(GVert), v.data(), GL_STREAM_DRAW);
			BindVertexLayout();
			SetOrigin(a_view, ox, oy, oz);
			glUniform1f(uLit, 0.0f);
			glUniform1f(uAlphaTest, 0.0f);
			glBindTexture(GL_TEXTURE_2D, 0);
			// A white 1x1 isn't bound: texture2D of texture 0 returns black in most drivers; the
			// vertex colour is black anyway, so tint the alpha only.
			glEnable(GL_BLEND);
			glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
			glLineWidth(2.0f);
			glDrawArrays(GL_LINES, 0, GLsizei(v.size()));
			glUniform1f(uLit, 1.0f);
		}
	}

	void ClearAll()
	{
		for (auto& [key, mesh] : gSections) {
			glDeleteBuffers(1, &mesh.vbo);
		}
		gSections.clear();
		gAvatar.valid = false;
		gScene.valid = false;
	}

	void HandleMessage(std::uint32_t a_type, const std::uint8_t* a_payload, std::uint32_t a_bytes)
	{
		if (!Init()) {
			return;
		}
		GLint prevBuf = 0, prevTex = 0, prevActive = 0;
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
		switch (a_type) {
			case proto::kRenAtlas: {
				if (a_bytes < sizeof(proto::RenAtlas)) {
					break;
				}
				const auto* hdr = reinterpret_cast<const proto::RenAtlas*>(a_payload);
				if (sizeof(proto::RenAtlas) + std::uint64_t(hdr->width) * hdr->height * 4 > a_bytes) {
					break;
				}
				if (gAtlas) {
					glDeleteTextures(1, &gAtlas);
				}
				gAtlas = NewTexture(int(hdr->width), int(hdr->height), a_payload + sizeof(proto::RenAtlas));
				gAtlasW = int(hdr->width), gAtlasH = int(hdr->height);
				Log("atlas %dx%d", gAtlasW, gAtlasH);
				break;
			}
			case proto::kRenAtlasRegion: {
				const auto* hdr = reinterpret_cast<const proto::RenAtlasRegion*>(a_payload);
				if (!gAtlas || a_bytes < sizeof(*hdr) + std::uint64_t(hdr->width) * hdr->height * 4) {
					break;
				}
				glBindTexture(GL_TEXTURE_2D, gAtlas);
				glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
				glTexSubImage2D(GL_TEXTURE_2D, 0, GLint(hdr->x), GLint(hdr->y), GLsizei(hdr->width), GLsizei(hdr->height), GL_RGBA, GL_UNSIGNED_BYTE, a_payload + sizeof(*hdr));
				break;
			}
			case proto::kRenSection: {
				const auto* hdr = reinterpret_cast<const proto::RenSection*>(a_payload);
				if (a_bytes < sizeof(*hdr) + std::uint64_t(hdr->vertexCount) * sizeof(proto::RenVertex)) {
					break;
				}
				UploadSection(*hdr, reinterpret_cast<const proto::RenVertex*>(a_payload + sizeof(*hdr)));
				break;
			}
			case proto::kRenClearAll:
				ClearAll();
				break;
			case proto::kRenTexture: {
				const auto* hdr = reinterpret_cast<const proto::RenTexture*>(a_payload);
				if (a_bytes < sizeof(*hdr) + std::uint64_t(hdr->width) * hdr->height * 4) {
					break;
				}
				if (auto it = gTextures.find(hdr->id); it != gTextures.end()) {
					glDeleteTextures(1, &it->second);
				}
				gTextures[hdr->id] = NewTexture(int(hdr->width), int(hdr->height), a_payload + sizeof(*hdr));
				break;
			}
			case proto::kRenAvatar: {
				const auto* hdr = reinterpret_cast<const proto::RenAvatar*>(a_payload);
				const auto* batches = reinterpret_cast<const proto::RenBatch*>(a_payload + sizeof(*hdr));
				const auto* verts = reinterpret_cast<const proto::RenVertex*>(batches + hdr->batchCount);
				if (a_bytes < sizeof(*hdr) + hdr->batchCount * sizeof(proto::RenBatch) + std::uint64_t(hdr->vertexCount) * sizeof(proto::RenVertex)) {
					break;
				}
				UploadDynamic(gAvatar, batches, hdr->batchCount, verts, hdr->vertexCount);
				break;
			}
			case proto::kRenScene: {
				const auto* hdr = reinterpret_cast<const proto::RenScene*>(a_payload);
				const auto* batches = reinterpret_cast<const proto::RenBatch*>(a_payload + sizeof(*hdr));
				const auto* verts = reinterpret_cast<const proto::RenVertex*>(batches + hdr->batchCount);
				if (a_bytes < sizeof(*hdr) + hdr->batchCount * sizeof(proto::RenBatch) + std::uint64_t(hdr->vertexCount) * sizeof(proto::RenVertex)) {
					break;
				}
				gSceneOrigin[0] = hdr->originX, gSceneOrigin[1] = hdr->originY, gSceneOrigin[2] = hdr->originZ;
				UploadDynamic(gScene, batches, hdr->batchCount, verts, hdr->vertexCount);
				break;
			}
			default:
				break;  // lights, NPC solids, dug cells, ragdoll: not used by c1
		}
		glBindBuffer(GL_ARRAY_BUFFER, prevBuf);
		glBindTexture(GL_TEXTURE_2D, prevTex);
		glActiveTexture(prevActive);
	}

	void DrawWorld(const View& a_view, const proto::WorldEntities* a_entities)
	{
		if (!Init() || !gAtlas) {
			return;
		}
		SavedState saved;
		saved.Save();

		glUseProgram(gProgram);
		glBindVertexArray(gVao);
		glViewport(0, 0, a_view.width, a_view.height);
		glDisable(GL_SCISSOR_TEST);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL);
		glDepthMask(GL_TRUE);
		glDisable(GL_CULL_FACE);
		glDisable(GL_POLYGON_OFFSET_FILL);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDisable(GL_BLEND);
		glActiveTexture(GL_TEXTURE0);
		glUniform1i(uTex, 0);
		glUniformMatrix4fv(uMvp, 1, GL_FALSE, a_view.viewProj);  // row-vector matrix == its transpose, column-major
		glUniform1f(uScale, float(kUnits));
		glUniform1f(uXAdjust, a_view.xAdjust);
		glUniform1f(uDay, a_view.daylight);
		glUniform1f(uLit, 1.0f);
		glUniform4f(uTint, 1, 1, 1, 1);

		// Camera right vector (for camera-facing sprites), from the view-projection's first column.
		const float camRight[3] = { a_view.viewProj[0], a_view.viewProj[4], a_view.viewProj[8] };

		// Pass 1: solid and cutout.
		glUniform1f(uAlphaTest, 0.1f);
		glBindTexture(GL_TEXTURE_2D, gAtlas);
		for (const auto& [key, mesh] : gSections) {
			if (mesh.opaque == 0) {
				continue;
			}
			int sx, sy, sz;
			UnpackSection(key, sx, sy, sz);
			glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
			BindVertexLayout();
			SetOrigin(a_view, sx * 16.0, sy * 16.0, sz * 16.0);
			glDrawArrays(GL_TRIANGLES, 0, mesh.opaque);
		}
		if (a_view.showAvatar) {
			DrawDynamic(a_view, gAvatar, a_view.feet, false);
		}
		DrawDynamic(a_view, gScene, gSceneOrigin, false);
		if (a_entities) {
			DrawEntities(a_view, *a_entities, camRight);
		}

		// Pass 2: translucent (water, stained glass, particles), blended, no depth writes.
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
		glDepthMask(GL_FALSE);
		glUniform1f(uAlphaTest, 0.004f);
		glBindTexture(GL_TEXTURE_2D, gAtlas);
		for (const auto& [key, mesh] : gSections) {
			if (mesh.translucent == 0) {
				continue;
			}
			int sx, sy, sz;
			UnpackSection(key, sx, sy, sz);
			glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
			BindVertexLayout();
			SetOrigin(a_view, sx * 16.0, sy * 16.0, sz * 16.0);
			glDrawArrays(GL_TRIANGLES, mesh.opaque, mesh.translucent);
		}
		if (a_view.showAvatar) {
			DrawDynamic(a_view, gAvatar, a_view.feet, true);
		}
		DrawDynamic(a_view, gScene, gSceneOrigin, true);
		if (a_entities) {
			DrawSelection(a_view, *a_entities);
		}

		glDisableVertexAttribArray(aPos);
		glDisableVertexAttribArray(aUV);
		glDisableVertexAttribArray(aColor);
		glDisableVertexAttribArray(aLight);
		saved.Restore();
	}

	bool SavePng(int a_w, int a_h, const char* a_path)
	{
		if (a_w <= 0 || a_h <= 0) {
			return false;
		}
		std::vector<std::uint8_t> pixels(std::size_t(a_w) * a_h * 3);
		GLint prevPack = 4;
		glGetIntegerv(GL_PACK_ALIGNMENT, &prevPack);
		GLint prevPackBuf = 0, readFbo = 0, drawFbo = 0;
		glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPackBuf);
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
		const GLenum errBefore = glGetError();
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadBuffer(GL_BACK);
		glReadPixels(0, 0, a_w, a_h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
		const GLenum errAfter = glGetError();
		std::uint8_t front[3] = {};
		glReadBuffer(GL_FRONT);
		glReadPixels(a_w / 2, a_h / 2, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, front);
		glReadBuffer(GL_BACK);
		const std::uint8_t* mid = &pixels[(std::size_t(a_h / 2) * a_w + a_w / 2) * 3];
		Log("readback: fbo r%d d%d pack %d, err %x/%x, centre back %d,%d,%d front %d,%d,%d", readFbo, drawFbo, prevPackBuf, errBefore, errAfter, mid[0], mid[1], mid[2],
			front[0], front[1], front[2]);
		glBindBuffer(GL_PIXEL_PACK_BUFFER, prevPackBuf);
		glPixelStorei(GL_PACK_ALIGNMENT, prevPack);
		return png::WriteRgbBottomUp(a_path, a_w, a_h, pixels.data());
	}

	void DrawOverlay(Link& a_link, const OverlayParams& a_p)
	{
		if (!Init()) {
			return;
		}
		SavedState saved;
		saved.Save();
		glActiveTexture(GL_TEXTURE0);
		if (a_link.AcquireOverlayFrame()) {
			const auto* hdr = a_link.FrontHeader();
			const int   w = int(hdr->width), h = int(hdr->height);
			if (w > 0 && h > 0 && w <= int(proto::kMaxOverlayW) && h <= int(proto::kMaxOverlayH)) {
				if (!gOverlayTex || w != gOverlayW || h != gOverlayH) {
					if (gOverlayTex) {
						glDeleteTextures(1, &gOverlayTex);
					}
					gOverlayTex = NewTexture(w, h, nullptr);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					gOverlayW = w, gOverlayH = h;
				}
				glBindTexture(GL_TEXTURE_2D, gOverlayTex);
				glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
				glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, a_link.FrontPixels());
				gOverlayBottomUp = (hdr->flags & 1) != 0;
				gHaveOverlay = true;
			}
		}
		if (!gHaveOverlay) {
			saved.Restore();
			return;
		}
		glUseProgram(gOverlayProgram);
		glBindVertexArray(gVao);
		glViewport(0, 0, a_p.width, a_p.height);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_DEPTH_TEST);
		glDepthMask(GL_FALSE);
		glDisable(GL_CULL_FACE);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
		glBindTexture(GL_TEXTURE_2D, gOverlayTex);
		glBindBuffer(GL_ARRAY_BUFFER, gQuadVbo);
		glEnableVertexAttribArray(oPos);
		glVertexAttribPointer(oPos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
		glUniform1i(oTex, 0);
		glUniform1f(oFlip, gOverlayBottomUp ? 0.0f : 1.0f);
		glUniform2f(oViewport, float(a_p.width), float(a_p.height));
		glUniform2f(oCursor, a_p.cursorX, a_p.cursorY);
		glUniform1f(oCursorOn, a_p.cursorOn ? 1.0f : 0.0f);
		// Minecraft's crosshair sits in a box at the centre (15 GUI pixels; the attack indicator under it).
		const float g = float(a_p.guiScale > 0 ? a_p.guiScale : 2);
		const float cx = a_p.width * 0.5f, cy = a_p.height * 0.5f;
		if (a_p.crosshair) {
			glUniform4f(oInvert, cx - 12.0f * g, cy - 12.0f * g, cx + 12.0f * g, cy + 28.0f * g);
		} else {
			glUniform4f(oInvert, 0, 0, 0, 0);
		}
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied straight from Minecraft
		glUniform1f(oMode, 0.0f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		if (a_p.crosshair && !a_p.hideCrosshair) {
			glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR);  // Minecraft's invert blend
			glUniform1f(oMode, 1.0f);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		glDisableVertexAttribArray(oPos);
		saved.Restore();
	}
}

namespace crashcraft::render
{
	void Stats(int& a_sections, long long& a_vertices, bool& a_atlas)
	{
		a_sections = int(gSections.size());
		a_vertices = 0;
		for (const auto& [key, mesh] : gSections) {
			a_vertices += mesh.opaque + mesh.translucent;
		}
		a_atlas = gAtlas != 0;
	}
}
