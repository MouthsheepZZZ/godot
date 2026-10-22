/**************************************************************************/
/*  lrt_transfer_codec.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                          */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md).  */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                   */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be        */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/


#include "lrt_transfer.h"

#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"

#include <algorithm>
#include <cstring>

namespace lrt {
namespace {

constexpr uint32_t TRANSFER_MAGIC = 0x3154524c; // LRT1
constexpr uint32_t TRANSFER_VERSION = 2;
constexpr size_t HEADER_BYTES = 68;
constexpr size_t BRICK_KEY_BYTES = 12;

void append_32(std::vector<uint8_t> &r_data, uint32_t p_value) {
	const size_t offset = r_data.size();
	r_data.resize(offset + 4);
	encode_uint32(p_value, r_data.data() + offset);
}

void append_64(std::vector<uint8_t> &r_data, uint64_t p_value) {
	const size_t offset = r_data.size();
	r_data.resize(offset + 8);
	encode_uint64(p_value, r_data.data() + offset);
}

void append_double(std::vector<uint8_t> &r_data, double p_value) {
	const size_t offset = r_data.size();
	r_data.resize(offset + 8);
	encode_double(p_value, r_data.data() + offset);
}

bool append_half(std::vector<uint8_t> &r_data, float p_value) {
	if (!std::isfinite(p_value) || std::abs(p_value) > 65504.0f) {
		return false;
	}
	const size_t offset = r_data.size();
	r_data.resize(offset + 2);
	encode_uint16(Math::make_half_float(p_value), r_data.data() + offset);
	return true;
}

bool valid_specification(const TransferField &p_field) {
	return std::isfinite(p_field.origin.x) && std::isfinite(p_field.origin.y) && std::isfinite(p_field.origin.z) &&
			std::isfinite(p_field.spacing) && p_field.spacing > 0.0 &&
			std::isfinite(p_field.support_radius) && p_field.support_radius > 0.0 && p_field.angular_samples > 0;
}

bool matching_specification(const TransferField &p_a, const TransferField &p_b) {
	return p_a.origin.x == p_b.origin.x && p_a.origin.y == p_b.origin.y && p_a.origin.z == p_b.origin.z &&
			p_a.spacing == p_b.spacing && p_a.support_radius == p_b.support_radius && p_a.angular_samples == p_b.angular_samples;
}

} // namespace

std::vector<uint8_t> encode_transfer_field(const TransferField &p_field, uint64_t p_signature, bool p_visibility) {
	if (!valid_specification(p_field) || p_field.bricks.size() > UINT32_MAX) {
		return {};
	}
	const size_t sample_bytes = p_visibility ? 8 : 120;
	std::vector<uint8_t> data;
	data.reserve(HEADER_BYTES + p_field.bricks.size() * (BRICK_KEY_BYTES + TransferField::BRICK_SAMPLES * sample_bytes));
	append_32(data, TRANSFER_MAGIC);
	append_32(data, TRANSFER_VERSION);
	append_32(data, p_visibility ? 1 : 0);
	append_64(data, p_signature);
	for (int axis = 0; axis < 3; axis++) {
		append_double(data, p_field.origin[axis]);
	}
	append_double(data, p_field.spacing);
	append_double(data, p_field.support_radius);
	append_32(data, p_field.angular_samples);
	append_32(data, uint32_t(p_field.bricks.size()));
	for (const auto &entry : p_field.bricks) {
		for (int axis = 0; axis < 3; axis++) {
			append_32(data, uint32_t(entry.first[axis]));
		}
		for (const TransferSample &sample : entry.second) {
			if (p_visibility) {
				// Normalize so both full visibility and zero survive FP16 exactly.
				const float scale = full_visibility()[0];
				for (float value : sample.visibility) {
					if (!append_half(data, value / scale)) {
						return {};
					}
				}
				continue;
			}
			for (int channel = 0; channel < 3; channel++) {
				for (float value : sample.reflection[channel]) {
					if (!append_half(data, value)) {
						return {};
					}
				}
				for (float value : sample.emission[channel]) {
					if (!append_half(data, value)) {
						return {};
					}
				}
			}
		}
	}
	return data;
}

bool decode_transfer_field(const std::vector<uint8_t> &p_data, uint64_t p_signature, bool p_visibility, TransferField &r_field) {
	if (p_data.size() < HEADER_BYTES || decode_uint32(p_data.data()) != TRANSFER_MAGIC ||
			decode_uint32(p_data.data() + 4) != TRANSFER_VERSION ||
			decode_uint32(p_data.data() + 8) != uint32_t(p_visibility) || decode_uint64(p_data.data() + 12) != p_signature) {
		return false;
	}
	TransferField decoded;
	size_t offset = 20;
	for (int axis = 0; axis < 3; axis++) {
		decoded.origin[axis] = decode_double(p_data.data() + offset);
		offset += 8;
	}
	decoded.spacing = decode_double(p_data.data() + offset);
	offset += 8;
	decoded.support_radius = decode_double(p_data.data() + offset);
	offset += 8;
	decoded.angular_samples = int(decode_uint32(p_data.data() + offset));
	offset += 4;
	const uint32_t brick_count = decode_uint32(p_data.data() + offset);
	offset += 4;
	const uint64_t brick_bytes = BRICK_KEY_BYTES + TransferField::BRICK_SAMPLES * (p_visibility ? 8u : 120u);
	if (!valid_specification(decoded) || HEADER_BYTES + uint64_t(brick_count) * brick_bytes != p_data.size() ||
			(r_field.spacing != 0.0 && !matching_specification(r_field, decoded))) {
		return false;
	}
	for (uint32_t brick = 0; brick < brick_count; brick++) {
		TransferField::BrickKey key;
		for (int axis = 0; axis < 3; axis++) {
			const uint32_t bits = decode_uint32(p_data.data() + offset);
			static_assert(sizeof(key[axis]) == sizeof(bits));
			memcpy(&key[axis], &bits, sizeof(bits));
			offset += 4;
		}
		if (decoded.bricks.find(key) != decoded.bricks.end()) {
			return false;
		}
		TransferField::Brick &values = decoded.bricks.try_emplace(key).first->second;
		for (TransferSample &sample : values) {
			auto read_half = [&](float &r_value) {
				r_value = Math::half_to_float(decode_uint16(p_data.data() + offset));
				offset += 2;
				return std::isfinite(r_value);
			};
			if (p_visibility) {
				const float scale = full_visibility()[0];
				for (float &value : sample.visibility) {
					if (!read_half(value)) {
						return false;
					}
					value *= scale;
				}
				continue;
			}
			for (int channel = 0; channel < 3; channel++) {
				for (float &value : sample.reflection[channel]) {
					if (!read_half(value)) {
						return false;
					}
				}
				for (float &value : sample.emission[channel]) {
					if (!read_half(value)) {
						return false;
					}
				}
			}
		}
	}
	// Merge channels only after the entire payload has passed validation.
	if (r_field.spacing == 0.0) {
		r_field = std::move(decoded);
		return true;
	}
	for (auto &entry : decoded.bricks) {
		TransferField::Brick &destination = r_field.bricks.try_emplace(entry.first).first->second;
		for (int i = 0; i < TransferField::BRICK_SAMPLES; i++) {
			if (p_visibility) {
				destination[i].visibility = entry.second[i].visibility;
			} else {
				destination[i].reflection = entry.second[i].reflection;
				destination[i].emission = entry.second[i].emission;
			}
		}
	}
	return true;
}

} // namespace lrt
