// Streams Super Mario 64's collision to Minecraft: exact triangles for the player's smooth collider and
// 1/8-block voxels for everything else (mobs, items, block placement). Voxelizer ported from SkyCraft
// (skse/src/Collision.cpp, MIT, chasmlol).
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "link.h"

namespace crashcraft
{
	struct McTri
	{
		float         v[9];  // Minecraft coordinates
		std::uint32_t flags{ 0 };
	};

	class Collision
	{
	public:
		static Collision& Get();
		static constexpr int kRegionSize = 8;

		void Start();
		// A new area: drop everything, then stream the area's static triangles (nearest first).
		void Reset(std::uint32_t a_epoch, std::vector<McTri>&& a_static, const double a_nearMc[3]);
		// Moving geometry near the player, a few times a second.
		void UpdateDynamic(const std::vector<McTri>& a_dynamic, const double a_playerMc[3]);
		[[nodiscard]] std::size_t Pending();
		// Cells Minecraft dug out of SM64's geometry (TNT craters), per 16-block section: bit x + 16z + 256y.
		// a_bits == nullptr: none left in that section. Main thread.
		void SetDug(int a_sx, int a_sy, int a_sz, const std::uint64_t* a_bits);
		void ClearDug();
		// Dug blocks whose cell overlaps [lo, hi] (any thread).
		void CollectDug(const float a_lo[3], const float a_hi[3], std::vector<std::array<int, 3>>& a_out);
		// Every dug block within the box [lo, lo + n) of whole blocks (any thread), as a 0/255 grid x + n*(y + n*z).
		void DugGrid(const int a_lo[3], int a_n, std::vector<std::uint8_t>& a_out);
		[[nodiscard]] std::uint32_t DugGeneration() const { return dugGeneration_; }

	private:
		using Key = std::uint64_t;
		static Key  RegionKey(int a_rx, int a_ry, int a_rz);
		static void Unpack(Key a_key, int& a_rx, int& a_ry, int& a_rz);

		struct Job
		{
			bool                 clear{ false };
			std::uint32_t        epoch{ 0 };
			int                  rx{ 0 }, ry{ 0 }, rz{ 0 };
			std::vector<McTri>   tris;
		};

		void WorkerLoop();
		void SendTriangles(const Job& a_job);
		void Voxelize(const Job& a_job);
		void Send(const std::vector<std::uint8_t>& a_payload, proto::ColType a_type);
		std::vector<McTri> TrisFor(Key a_key) const;

		std::mutex              mutex_;
		std::condition_variable cv_;
		std::deque<Job>         queue_;
		std::thread             worker_;
		std::uint32_t           epoch_{ 0 };

		// Main thread only.
		std::vector<McTri>                              static_;
		std::unordered_map<Key, std::vector<std::uint32_t>> staticByRegion_;
		std::unordered_map<Key, std::vector<McTri>>     dynamicByRegion_;
		std::unordered_map<Key, std::uint64_t>          dynamicHash_;

		mutable std::mutex                                     dugMutex_;
		std::unordered_map<Key, std::array<std::uint64_t, 64>> dug_;  // section -> bits
		std::atomic<std::uint32_t>                             dugGeneration_{ 0 };
	};
}
