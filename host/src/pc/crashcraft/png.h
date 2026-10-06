// Minimal PNG writer for debug screenshots: RGB8, zlib "stored" blocks (no compression library needed).
#pragma once

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace crashcraft::png
{
	inline std::uint32_t Crc(const std::uint8_t* a_data, std::size_t a_len, std::uint32_t a_crc = 0)
	{
		static std::uint32_t table[256];
		static bool          built = false;
		if (!built) {
			for (std::uint32_t n = 0; n < 256; ++n) {
				std::uint32_t c = n;
				for (int k = 0; k < 8; ++k) {
					c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
				}
				table[n] = c;
			}
			built = true;
		}
		std::uint32_t c = a_crc ^ 0xFFFFFFFFu;
		for (std::size_t i = 0; i < a_len; ++i) {
			c = table[(c ^ a_data[i]) & 0xFF] ^ (c >> 8);
		}
		return c ^ 0xFFFFFFFFu;
	}

	// a_rgb: rows bottom-up (as glReadPixels returns them), 3 bytes per pixel, tightly packed.
	inline bool WriteRgbBottomUp(const char* a_path, int a_w, int a_h, const std::uint8_t* a_rgb)
	{
		std::vector<std::uint8_t> raw;
		raw.reserve(std::size_t(a_w * 3 + 1) * a_h);
		for (int y = a_h - 1; y >= 0; --y) {
			raw.push_back(0);  // filter: none
			const std::uint8_t* row = a_rgb + std::size_t(y) * a_w * 3;
			raw.insert(raw.end(), row, row + std::size_t(a_w) * 3);
		}
		std::vector<std::uint8_t> z = { 0x78, 0x01 };
		std::uint32_t             a = 1, b = 0;
		for (std::uint8_t v : raw) {
			a = (a + v) % 65521;
			b = (b + a) % 65521;
		}
		for (std::size_t pos = 0; pos < raw.size() || raw.empty();) {
			const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
			const bool        last = pos + n >= raw.size();
			z.push_back(last ? 1 : 0);
			z.push_back(std::uint8_t(n)), z.push_back(std::uint8_t(n >> 8));
			z.push_back(std::uint8_t(~n)), z.push_back(std::uint8_t(~n >> 8));
			z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
			pos += n;
			if (last) {
				break;
			}
		}
		const std::uint32_t adler = (b << 16) | a;
		for (int s = 24; s >= 0; s -= 8) {
			z.push_back(std::uint8_t(adler >> s));
		}

		FILE* f = std::fopen(a_path, "wb");
		if (!f) {
			return false;
		}
		auto be32 = [](std::uint8_t* p, std::uint32_t v) { p[0] = std::uint8_t(v >> 24), p[1] = std::uint8_t(v >> 16), p[2] = std::uint8_t(v >> 8), p[3] = std::uint8_t(v); };
		auto chunk = [&](const char* type, const std::uint8_t* data, std::uint32_t len) {
			std::uint8_t hdr[8];
			be32(hdr, len);
			std::memcpy(hdr + 4, type, 4);
			std::fwrite(hdr, 1, 8, f);
			if (len) {
				std::fwrite(data, 1, len, f);
			}
			std::uint32_t crc = Crc(hdr + 4, 4);
			crc = Crc(data, len, crc);
			std::uint8_t c[4];
			be32(c, crc);
			std::fwrite(c, 1, 4, f);
		};
		static const std::uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		std::fwrite(sig, 1, 8, f);
		std::uint8_t ihdr[13];
		be32(ihdr, std::uint32_t(a_w));
		be32(ihdr + 4, std::uint32_t(a_h));
		ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;  // 8-bit RGB
		chunk("IHDR", ihdr, 13);
		chunk("IDAT", z.data(), std::uint32_t(z.size()));
		chunk("IEND", nullptr, 0);
		std::fclose(f);
		return true;
	}
}
