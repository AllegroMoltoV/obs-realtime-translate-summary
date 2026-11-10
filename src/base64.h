#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rts {

inline std::string base64_encode(const uint8_t *data, size_t size)
{
	static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((size + 2) / 3 * 4);

	size_t i = 0;
	for (; i + 2 < size; i += 3) {
		const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
		out.push_back(kTable[(n >> 18) & 0x3F]);
		out.push_back(kTable[(n >> 12) & 0x3F]);
		out.push_back(kTable[(n >> 6) & 0x3F]);
		out.push_back(kTable[n & 0x3F]);
	}
	if (i + 1 == size) {
		const uint32_t n = uint32_t(data[i]) << 16;
		out.push_back(kTable[(n >> 18) & 0x3F]);
		out.push_back(kTable[(n >> 12) & 0x3F]);
		out += "==";
	} else if (i + 2 == size) {
		const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
		out.push_back(kTable[(n >> 18) & 0x3F]);
		out.push_back(kTable[(n >> 12) & 0x3F]);
		out.push_back(kTable[(n >> 6) & 0x3F]);
		out.push_back('=');
	}
	return out;
}

} // namespace rts
