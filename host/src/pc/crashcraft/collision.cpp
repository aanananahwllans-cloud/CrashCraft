#include "collision.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "clip.h"

namespace crashcraft
{
	namespace
	{
		constexpr int   kGrid = Collision::kRegionSize * 8;  // voxels per region edge (64)
		constexpr float kSteepMin = 0.1f;                    // |n.y| below this is a wall: keep it fine-grained
		constexpr float kSteepMax = 0.643f;                  // steeper than ~50 degrees: block-coarsened

		void Sub(const float* a, const float* b, float* o) { o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2]; }
		float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
		void Cross(const float* a, const float* b, float* o)
		{
			o[0] = a[1] * b[2] - a[2] * b[1];
			o[1] = a[2] * b[0] - a[0] * b[2];
			o[2] = a[0] * b[1] - a[1] * b[0];
		}

		bool AxisTest(const float* v0, const float* v1, const float* v2, const float* axis, float h)
		{
			const float p0 = Dot(v0, axis), p1 = Dot(v1, axis), p2 = Dot(v2, axis);
			const float r = h * (std::fabs(axis[0]) + std::fabs(axis[1]) + std::fabs(axis[2]));
			return !(std::min({ p0, p1, p2 }) > r || std::max({ p0, p1, p2 }) < -r);
		}

		// Akenine-Moller triangle / cube overlap (cube centre c, half size h).
		bool TriBoxOverlap(const float* c, float h, const float* ta, const float* tb, const float* tc, const float* n)
		{
			float v0[3], v1[3], v2[3];
			Sub(ta, c, v0);
			Sub(tb, c, v1);
			Sub(tc, c, v2);
			for (int i = 0; i < 3; ++i) {
				const float mn = std::min({ v0[i], v1[i], v2[i] }), mx = std::max({ v0[i], v1[i], v2[i] });
				if (mn > h || mx < -h) {
					return false;
				}
			}
			const float d = Dot(n, v0);
			const float r = h * (std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]));
			if (std::fabs(d) > r) {
				return false;
			}
			float e[3][3];
			Sub(v1, v0, e[0]);
			Sub(v2, v1, e[1]);
			Sub(v0, v2, e[2]);
			static constexpr float kAxes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
			for (auto& edge : e) {
				for (auto& unit : kAxes) {
					float axis[3];
					Cross(edge, unit, axis);
					if (!AxisTest(v0, v1, v2, axis, h)) {
						return false;
					}
				}
			}
			return true;
		}

		int FloorDiv(int v, int d) { return v >= 0 ? v / d : -((-v + d - 1) / d); }

		void TriBounds(const McTri& t, float lo[3], float hi[3])
		{
			for (int i = 0; i < 3; ++i) {
				lo[i] = std::min({ t.v[i], t.v[3 + i], t.v[6 + i] });
				hi[i] = std::max({ t.v[i], t.v[3 + i], t.v[6 + i] });
			}
		}

		template <class Fn>
		void ForRegions(const McTri& t, Fn&& a_fn)
		{
			float lo[3], hi[3];
			TriBounds(t, lo, hi);
			constexpr float eps = 0.01f;
			const int       r0[3] = { FloorDiv(int(std::floor(lo[0] - eps)), Collision::kRegionSize), FloorDiv(int(std::floor(lo[1] - eps)), Collision::kRegionSize),
                FloorDiv(int(std::floor(lo[2] - eps)), Collision::kRegionSize) };
			const int r1[3] = { FloorDiv(int(std::floor(hi[0] + eps)), Collision::kRegionSize), FloorDiv(int(std::floor(hi[1] + eps)), Collision::kRegionSize),
				FloorDiv(int(std::floor(hi[2] + eps)), Collision::kRegionSize) };
			for (int rx = r0[0]; rx <= r1[0]; ++rx) {
				for (int ry = r0[1]; ry <= r1[1]; ++ry) {
					for (int rz = r0[2]; rz <= r1[2]; ++rz) {
						a_fn(rx, ry, rz);
					}
				}
			}
		}
	}

	Collision& Collision::Get()
	{
		static Collision collision;
		return collision;
	}

	Collision::Key Collision::RegionKey(int a_rx, int a_ry, int a_rz)
	{
		return (std::uint64_t(a_rx & 0x1FFFFF) << 42) | (std::uint64_t(a_ry & 0x1FFFFF) << 21) | std::uint64_t(a_rz & 0x1FFFFF);
	}

	void Collision::Unpack(Key a_key, int& a_rx, int& a_ry, int& a_rz)
	{
		auto unpack = [](Key k, int shift) {
			const int v = static_cast<int>((k >> shift) & 0x1FFFFF);
			return v >= 0x100000 ? v - 0x200000 : v;
		};
		a_rx = unpack(a_key, 42);
		a_ry = unpack(a_key, 21);
		a_rz = unpack(a_key, 0);
	}

	void Collision::Start()
	{
		if (!worker_.joinable()) {
			worker_ = std::thread([this] { WorkerLoop(); });
			worker_.detach();
		}
	}

	std::size_t Collision::Pending()
	{
		std::lock_guard lock(mutex_);
		return queue_.size();
	}

	void Collision::ClearDug()
	{
		std::lock_guard lock(dugMutex_);
		dug_.clear();
		++dugGeneration_;
	}

	void Collision::SetDug(int a_sx, int a_sy, int a_sz, const std::uint64_t* a_bits)
	{
		const Key key = RegionKey(a_sx, a_sy, a_sz);
		{
			std::lock_guard lock(dugMutex_);
			if (a_bits) {
				std::array<std::uint64_t, 64> bits;
				std::memcpy(bits.data(), a_bits, sizeof(bits));
				auto it = dug_.find(key);
				if (it != dug_.end() && it->second == bits) {
					return;
				}
				dug_[key] = bits;
			} else if (!dug_.erase(key)) {
				return;
			}
			++dugGeneration_;
		}
		// The 2x2x2 collision regions of that section, ahead of everything else.
		std::vector<Job> jobs;
		for (int dy = 0; dy < 2; ++dy) {
			for (int dz = 0; dz < 2; ++dz) {
				for (int dx = 0; dx < 2; ++dx) {
					Job job;
					job.rx = a_sx * 2 + dx, job.ry = a_sy * 2 + dy, job.rz = a_sz * 2 + dz;
					job.tris = TrisFor(RegionKey(job.rx, job.ry, job.rz));
					jobs.push_back(std::move(job));
				}
			}
		}
		std::lock_guard lock(mutex_);
		for (auto& job : jobs) {
			job.epoch = epoch_;
			auto at = queue_.begin();
			if (at != queue_.end() && at->clear) {
				++at;
			}
			queue_.insert(at, std::move(job));
		}
		cv_.notify_one();
	}

	void Collision::CollectDug(const float a_lo[3], const float a_hi[3], std::vector<std::array<int, 3>>& a_out)
	{
		std::lock_guard lock(dugMutex_);
		if (dug_.empty()) {
			return;
		}
		const int lo[3] = { int(std::floor(a_lo[0])), int(std::floor(a_lo[1])), int(std::floor(a_lo[2])) };
		const int hi[3] = { int(std::floor(a_hi[0])), int(std::floor(a_hi[1])), int(std::floor(a_hi[2])) };
		for (const auto& [key, bits] : dug_) {
			int sx, sy, sz;
			Unpack(key, sx, sy, sz);
			if (sx * 16 > hi[0] || sx * 16 + 15 < lo[0] || sy * 16 > hi[1] || sy * 16 + 15 < lo[1] || sz * 16 > hi[2] || sz * 16 + 15 < lo[2]) {
				continue;
			}
			for (int i = 0; i < 4096; ++i) {
				if (bits[i >> 6] & (1ull << (i & 63))) {
					const int x = sx * 16 + (i & 15), z = sz * 16 + ((i >> 4) & 15), y = sy * 16 + (i >> 8);
					if (x >= lo[0] && x <= hi[0] && y >= lo[1] && y <= hi[1] && z >= lo[2] && z <= hi[2]) {
						a_out.push_back({ x, y, z });
					}
				}
			}
		}
	}

	void Collision::DugGrid(const int a_lo[3], int a_n, std::vector<std::uint8_t>& a_out)
	{
		a_out.assign(std::size_t(a_n) * a_n * a_n, 0);
		std::vector<std::array<int, 3>> cubes;
		const float lo[3] = { float(a_lo[0]), float(a_lo[1]), float(a_lo[2]) };
		const float hi[3] = { float(a_lo[0] + a_n - 1), float(a_lo[1] + a_n - 1), float(a_lo[2] + a_n - 1) };
		CollectDug(lo, hi, cubes);
		for (const auto& cube : cubes) {
			const int x = cube[0] - a_lo[0], y = cube[1] - a_lo[1], z = cube[2] - a_lo[2];
			a_out[std::size_t(x) + std::size_t(a_n) * (std::size_t(y) + std::size_t(a_n) * z)] = 255;
		}
	}

	void Collision::Reset(std::uint32_t a_epoch, std::vector<McTri>&& a_static, const double a_nearMc[3])
	{
		static_ = std::move(a_static);
		staticByRegion_.clear();
		dynamicByRegion_.clear();
		dynamicHash_.clear();
		int lo[3] = { INT32_MAX, INT32_MAX, INT32_MAX }, hi[3] = { INT32_MIN, INT32_MIN, INT32_MIN };
		for (std::uint32_t i = 0; i < static_.size(); ++i) {
			ForRegions(static_[i], [&](int rx, int ry, int rz) {
				staticByRegion_[RegionKey(rx, ry, rz)].push_back(i);
				lo[0] = std::min(lo[0], rx), lo[1] = std::min(lo[1], ry), lo[2] = std::min(lo[2], rz);
				hi[0] = std::max(hi[0], rx), hi[1] = std::max(hi[1], ry), hi[2] = std::max(hi[2], rz);
			});
		}

		// Every region of the area's box, empty ones included: Minecraft only releases a player
		// once the regions around them are known, even in mid-air.
		std::vector<std::pair<double, Key>> order;
		if (!static_.empty()) {
			for (int rx = lo[0] - 1; rx <= hi[0] + 1; ++rx) {
				for (int ry = lo[1] - 2; ry <= hi[1] + 2; ++ry) {
					for (int rz = lo[2] - 1; rz <= hi[2] + 1; ++rz) {
						const double cx = (rx + 0.5) * kRegionSize - a_nearMc[0], cy = (ry + 0.5) * kRegionSize - a_nearMc[1],
									 cz = (rz + 0.5) * kRegionSize - a_nearMc[2];
						order.emplace_back(cx * cx + cy * cy * 4.0 + cz * cz, RegionKey(rx, ry, rz));
					}
				}
			}
		}
		std::sort(order.begin(), order.end());

		std::lock_guard lock(mutex_);
		epoch_ = a_epoch;
		queue_.clear();
		Job clear;
		clear.clear = true;
		clear.epoch = a_epoch;
		queue_.push_back(std::move(clear));
		for (const auto& [dist, key] : order) {
			Job job;
			job.epoch = a_epoch;
			Unpack(key, job.rx, job.ry, job.rz);
			job.tris = TrisFor(key);
			queue_.push_back(std::move(job));
		}
		Log("collision: epoch %u, %zu triangles in %zu regions (%zu sent with empties)", a_epoch, static_.size(), staticByRegion_.size(), order.size());
		cv_.notify_one();
	}

	std::vector<McTri> Collision::TrisFor(Key a_key) const
	{
		std::vector<McTri> out;
		if (auto it = staticByRegion_.find(a_key); it != staticByRegion_.end()) {
			out.reserve(it->second.size());
			for (auto i : it->second) {
				out.push_back(static_[i]);
			}
		}
		if (auto it = dynamicByRegion_.find(a_key); it != dynamicByRegion_.end()) {
			out.insert(out.end(), it->second.begin(), it->second.end());
		}
		return out;
	}

	void Collision::UpdateDynamic(const std::vector<McTri>& a_dynamic, const double a_playerMc[3])
	{
		constexpr double kRange = 40.0;
		std::unordered_map<Key, std::vector<McTri>> fresh;
		for (const auto& t : a_dynamic) {
			float lo[3], hi[3];
			TriBounds(t, lo, hi);
			if (lo[0] > a_playerMc[0] + kRange || hi[0] < a_playerMc[0] - kRange || lo[1] > a_playerMc[1] + kRange || hi[1] < a_playerMc[1] - kRange ||
				lo[2] > a_playerMc[2] + kRange || hi[2] < a_playerMc[2] - kRange) {
				continue;
			}
			ForRegions(t, [&](int rx, int ry, int rz) { fresh[RegionKey(rx, ry, rz)].push_back(t); });
		}
		std::unordered_set<Key> touched;
		for (const auto& [key, tris] : dynamicByRegion_) {
			touched.insert(key);
		}
		for (const auto& [key, tris] : fresh) {
			touched.insert(key);
		}
		dynamicByRegion_ = std::move(fresh);

		std::vector<Job> jobs;
		for (Key key : touched) {
			std::uint64_t hash = 1469598103934665603ull;
			if (auto it = dynamicByRegion_.find(key); it != dynamicByRegion_.end()) {
				for (const auto& t : it->second) {
					for (float f : t.v) {
						std::uint32_t bits;
						std::memcpy(&bits, &f, 4);
						hash = (hash ^ bits) * 1099511628211ull;
					}
				}
			}
			auto& before = dynamicHash_[key];
			if (before == hash) {
				continue;
			}
			before = hash;
			Job job;
			Unpack(key, job.rx, job.ry, job.rz);
			job.tris = TrisFor(key);
			jobs.push_back(std::move(job));
		}
		if (jobs.empty()) {
			return;
		}
		std::lock_guard lock(mutex_);
		for (auto& job : jobs) {
			job.epoch = epoch_;
			// Ahead of the level's own regions: these are right next to the player.
			auto at = queue_.begin();
			if (at != queue_.end() && at->clear) {
				++at;
			}
			queue_.insert(at, std::move(job));
		}
		cv_.notify_one();
	}

	void Collision::WorkerLoop()
	{
		while (true) {
			Job job;
			{
				std::unique_lock lock(mutex_);
				cv_.wait(lock, [this] { return !queue_.empty(); });
				job = std::move(queue_.front());
				queue_.pop_front();
			}
			if (job.clear) {
				std::vector<std::uint8_t> payload(4);
				std::memcpy(payload.data(), &job.epoch, 4);
				Send(payload, proto::kColClear);
				continue;
			}
			{
				std::lock_guard lock(mutex_);
				if (job.epoch != epoch_) {
					continue;
				}
			}
			SendTriangles(job);
			Voxelize(job);
		}
	}

	void Collision::Send(const std::vector<std::uint8_t>& a_payload, proto::ColType a_type)
	{
		auto& link = Link::Get();
		for (int attempt = 0; attempt < 4000; ++attempt) {
			if (link.WriteCollision(a_type, a_payload.data(), static_cast<std::uint32_t>(a_payload.size()))) {
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));  // ring full: Minecraft is behind
		}
		Log("collision ring stayed full; dropped a message");
	}

	void Collision::SendTriangles(const Job& a_job)
	{
		proto::ColRegion header{};
		header.minX = a_job.rx * kRegionSize;
		header.minY = a_job.ry * kRegionSize;
		header.minZ = a_job.rz * kRegionSize;
		header.maxX = header.minX + kRegionSize - 1;
		header.maxY = header.minY + kRegionSize - 1;
		header.maxZ = header.minZ + kRegionSize - 1;
		header.epoch = a_job.epoch;

		std::vector<proto::ColTri> out;
		out.reserve(a_job.tris.size());
		auto add = [&](const float* a_v, std::uint32_t a_flags) {
			proto::ColTri t{};
			std::memcpy(t.v, a_v, sizeof(t.v));
			t.flags = a_flags;
			out.push_back(t);
		};
		// TNT craters: dug blocks cut out of the diggable surfaces (the surface as it was goes too, as a
		// ghost: Minecraft tells from it what's solid behind a hole, and lines the hole with blocks).
		std::vector<Clip::Box> boxes;
		{
			float dlo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, dhi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
			for (const auto& t : a_job.tris) {
				if (t.flags & proto::kTriDiggable) {
					for (int k = 0; k < 3; ++k) {
						dlo[k] = std::min({ dlo[k], t.v[k], t.v[3 + k], t.v[6 + k] });
						dhi[k] = std::max({ dhi[k], t.v[k], t.v[3 + k], t.v[6 + k] });
					}
				}
			}
			if (dlo[0] <= dhi[0]) {
				std::vector<Clip::Cube> cubes;
				CollectDug(dlo, dhi, cubes);
				boxes = Clip::Merge(cubes);
			}
		}
		std::vector<Clip::Box>  nearBoxes;
		std::vector<Clip::Poly> pieces;
		for (const auto& t : a_job.tris) {
			if ((t.flags & proto::kTriDiggable) && !boxes.empty()) {
				float tlo[3], thi[3];
				for (int k = 0; k < 3; ++k) {
					tlo[k] = std::min({ t.v[k], t.v[3 + k], t.v[6 + k] });
					thi[k] = std::max({ t.v[k], t.v[3 + k], t.v[6 + k] });
				}
				nearBoxes.clear();
				for (const auto& b : boxes) {
					if (thi[0] >= b.lo[0] && tlo[0] <= b.hi[0] && thi[1] >= b.lo[1] && tlo[1] <= b.hi[1] && thi[2] >= b.lo[2] && tlo[2] <= b.hi[2]) {
						nearBoxes.push_back(b);
					}
				}
				if (!nearBoxes.empty()) {
					add(t.v, t.flags | proto::kTriGhost);
					pieces.clear();
					Clip::Subtract(Clip::FromTriangle(t.v, t.v + 3, t.v + 6), nearBoxes, pieces);
					for (const auto& piece : pieces) {
						for (std::size_t v = 1; v + 1 < piece.size(); ++v) {
							const float part[9] = { piece[0].p[0], piece[0].p[1], piece[0].p[2], piece[v].p[0], piece[v].p[1], piece[v].p[2],
								piece[v + 1].p[0], piece[v + 1].p[1], piece[v + 1].p[2] };
							add(part, t.flags);
						}
					}
					continue;
				}
			}
			add(t.v, t.flags);
		}
		header.count = static_cast<std::uint32_t>(out.size());
		std::vector<std::uint8_t> payload(sizeof(header) + out.size() * sizeof(proto::ColTri));
		std::memcpy(payload.data(), &header, sizeof(header));
		if (!out.empty()) {
			std::memcpy(payload.data() + sizeof(header), out.data(), out.size() * sizeof(proto::ColTri));
		}
		Send(payload, proto::kColTris);
	}

	void Collision::Voxelize(const Job& a_job)
	{
		constexpr int              G = kGrid;
		std::vector<std::uint64_t> solid(G * G, 0), steep(G * G, 0);
		std::vector<std::uint64_t> digSolid(G * G, 0), digSteep(G * G, 0);  // diggable geometry
		auto set = [&](std::vector<std::uint64_t>& a_grid, int x, int y, int z) { a_grid[y * G + z] |= 1ull << x; };

		const float ox = float(a_job.rx * kRegionSize), oy = float(a_job.ry * kRegionSize), oz = float(a_job.rz * kRegionSize);
		auto        toVoxel = [&](const float* a_mc, float* a_out) {
            a_out[0] = (a_mc[0] - ox) * 8.0f;
            a_out[1] = (a_mc[1] - oy) * 8.0f;
            a_out[2] = (a_mc[2] - oz) * 8.0f;
		};
		auto clampLo = [](float v) { return std::clamp(static_cast<int>(std::floor(v)), 0, G - 1); };
		auto clampHi = [](float v) { return std::clamp(static_cast<int>(std::ceil(v)) - 1, 0, G - 1); };

		for (const auto& tri : a_job.tris) {
			float a[3], b[3], c[3];
			toVoxel(tri.v, a);
			toVoxel(tri.v + 3, b);
			toVoxel(tri.v + 6, c);
			float lo[3], hi[3];
			for (int i = 0; i < 3; ++i) {
				lo[i] = std::min({ a[i], b[i], c[i] });
				hi[i] = std::max({ a[i], b[i], c[i] });
			}
			if (hi[0] < 0 || hi[1] < 0 || hi[2] < 0 || lo[0] > G || lo[1] > G || lo[2] > G) {
				continue;
			}
			float e1[3], e2[3], n[3];
			Sub(b, a, e1);
			Sub(c, a, e2);
			Cross(e1, e2, n);
			const float len = std::sqrt(Dot(n, n));
			if (len < 1e-9f) {
				continue;
			}
			n[0] /= len, n[1] /= len, n[2] /= len;
			const float ny = std::fabs(n[1]);
			const bool  flat = ny >= kSteepMax || ny < kSteepMin;
			auto&       grid = (tri.flags & proto::kTriDiggable) ? (flat ? digSolid : digSteep) : (flat ? solid : steep);

			int dom = 0;
			if (std::fabs(n[1]) > std::fabs(n[dom])) dom = 1;
			if (std::fabs(n[2]) > std::fabs(n[dom])) dom = 2;
			const int   u = (dom + 1) % 3, v = (dom + 2) % 3;
			const float d = Dot(n, a);
			const float r = 0.5f * (std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]));
			const int   iu0 = clampLo(lo[u]), iu1 = clampHi(hi[u]), iv0 = clampLo(lo[v]), iv1 = clampHi(hi[v]);
			const int   id0 = clampLo(lo[dom]), id1 = clampHi(hi[dom]);
			for (int iu = iu0; iu <= iu1; ++iu) {
				for (int iv = iv0; iv <= iv1; ++iv) {
					const float cu = iu + 0.5f, cv = iv + 0.5f;
					const float s0 = (d - r - n[u] * cu - n[v] * cv) / n[dom];
					const float s1 = (d + r - n[u] * cu - n[v] * cv) / n[dom];
					int         a0 = std::max(id0, static_cast<int>(std::floor(std::min(s0, s1) - 0.5f)));
					int         a1 = std::min(id1, static_cast<int>(std::ceil(std::max(s0, s1) - 0.5f)));
					for (int id = a0; id <= a1; ++id) {
						float cen[3];
						cen[dom] = id + 0.5f;
						cen[u] = cu;
						cen[v] = cv;
						if (TriBoxOverlap(cen, 0.5f, a, b, c, n)) {
							int p[3];
							p[dom] = id, p[u] = iu, p[v] = iv;
							set(grid, p[0], p[1], p[2]);
						}
					}
				}
			}
		}

		// Steep (50-84 degree) surfaces snap to whole-block footprints, so Minecraft's own step-up
		// and jump rules decide what is climbable, like a cliff made of blocks.
		auto coarsen = [&](const std::vector<std::uint64_t>& a_steep, std::vector<std::uint64_t>& a_solid) {
			for (int by = 0; by < kRegionSize; ++by) {
				for (int bz = 0; bz < kRegionSize; ++bz) {
					for (int bx = 0; bx < kRegionSize; ++bx) {
						const std::uint64_t xmask = 0xFFull << (bx * 8);
						int                 minY = 99, maxY = -1;
						for (int y = by * 8; y < by * 8 + 8; ++y) {
							for (int z = bz * 8; z < bz * 8 + 8; ++z) {
								if (a_steep[y * G + z] & xmask) {
									minY = std::min(minY, y);
									maxY = std::max(maxY, y);
								}
							}
						}
						if (maxY < 0) {
							continue;
						}
						for (int y = minY; y <= maxY; ++y) {
							for (int z = bz * 8; z < bz * 8 + 8; ++z) {
								a_solid[y * G + z] |= xmask;
							}
						}
					}
				}
			}
		};
		coarsen(steep, solid);
		coarsen(digSteep, digSolid);

		// Dug blocks: the diggable geometry in them is gone.
		{
			const float rlo[3] = { ox + 0.5f, oy + 0.5f, oz + 0.5f };
			const float rhi[3] = { ox + kRegionSize - 0.5f, oy + kRegionSize - 0.5f, oz + kRegionSize - 0.5f };
			std::vector<std::array<int, 3>> dug;
			CollectDug(rlo, rhi, dug);
			for (const auto& cube : dug) {
				const int bx = cube[0] - int(ox), by = cube[1] - int(oy), bz = cube[2] - int(oz);
				if (bx < 0 || by < 0 || bz < 0 || bx >= kRegionSize || by >= kRegionSize || bz >= kRegionSize) {
					continue;
				}
				const std::uint64_t keep = ~(0xFFull << (bx * 8));
				for (int y = by * 8; y < by * 8 + 8; ++y) {
					for (int z = bz * 8; z < bz * 8 + 8; ++z) {
						digSolid[y * G + z] &= keep;
					}
				}
			}
		}
		for (std::size_t i = 0; i < solid.size(); ++i) {
			solid[i] |= digSolid[i];
		}

		std::vector<proto::ColBlock> blocks;
		for (int by = 0; by < kRegionSize; ++by) {
			for (int bz = 0; bz < kRegionSize; ++bz) {
				for (int bx = 0; bx < kRegionSize; ++bx) {
					proto::ColBlock blk{};
					bool            any = false;
					for (int sy = 0; sy < 8; ++sy) {
						std::uint64_t layer = 0;
						for (int sz = 0; sz < 8; ++sz) {
							const auto row = (solid[(by * 8 + sy) * G + (bz * 8 + sz)] >> (bx * 8)) & 0xFF;
							layer |= row << (sz * 8);
						}
						blk.bits[sy] = layer;
						any |= layer != 0;
					}
					if (any) {
						blk.x = a_job.rx * kRegionSize + bx;
						blk.y = a_job.ry * kRegionSize + by;
						blk.z = a_job.rz * kRegionSize + bz;
						blocks.push_back(blk);
					}
				}
			}
		}

		proto::ColRegion header{};
		header.minX = a_job.rx * kRegionSize;
		header.minY = a_job.ry * kRegionSize;
		header.minZ = a_job.rz * kRegionSize;
		header.maxX = header.minX + kRegionSize - 1;
		header.maxY = header.minY + kRegionSize - 1;
		header.maxZ = header.minZ + kRegionSize - 1;
		header.epoch = a_job.epoch;
		header.count = static_cast<std::uint32_t>(blocks.size());
		std::vector<std::uint8_t> payload(sizeof(header) + blocks.size() * sizeof(proto::ColBlock));
		std::memcpy(payload.data(), &header, sizeof(header));
		if (!blocks.empty()) {
			std::memcpy(payload.data() + sizeof(header), blocks.data(), blocks.size() * sizeof(proto::ColBlock));
		}
		Send(payload, proto::kColRegion);
	}
}
