// The SM64 end of the shared-memory link (port of SkyCraft's skse/src/Link.h, MIT, chasmlol).
// SM64 creates the mapping; Minecraft opens it.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <functional>

#include "crashcraft_protocol.h"

namespace crashcraft
{
	void Log(const char* a_fmt, ...);
	void LogV(const char* a_fmt, va_list a_args);

	class Link
	{
	public:
		static Link& Get();

		bool Create();
		[[nodiscard]] bool Valid() const { return base_ != nullptr; }

		[[nodiscard]] bool          McAlive() const;
		void                        Heartbeat();
		[[nodiscard]] std::uint32_t McPid() const;

		void WriteSkyState(const proto::SkyState& a_state);
		void WriteWaterGrid(const proto::WaterGrid& a_grid);
		bool ReadMcState(proto::McState& a_out) const;

		void PushInput(proto::InputType a_type, std::uint16_t a_code, std::int32_t a_a = 0, std::int32_t a_b = 0, std::int32_t a_c = 0);
		bool WriteCollision(proto::ColType a_type, const void* a_payload, std::uint32_t a_bytes);
		void WriteActors(const proto::ActorRecord* a_records, std::uint32_t a_count);
		bool PopEvent(proto::McEvent& a_out);
		bool ReadWorldEntities(proto::WorldEntities& a_out) const;
		void DrainRender(const std::function<void(std::uint32_t, const std::uint8_t*, std::uint32_t)>& a_fn, std::uint64_t a_maxBytes);

		bool                                       AcquireOverlayFrame();
		void                                       ResetOverlay();
		[[nodiscard]] const std::uint8_t*          FrontPixels() const;
		[[nodiscard]] const proto::OverlaySlotHdr* FrontHeader() const;

	private:
		template <class T>
		T* At(std::uint64_t a_off) const { return reinterpret_cast<T*>(base_ + a_off); }

		void*         mapping_{ nullptr };
		std::uint8_t* base_{ nullptr };
		std::uint32_t overlayFront_{ 2 };
	};
}
