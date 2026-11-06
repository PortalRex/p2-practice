// Bounds-checked little-endian byte and bit readers (the C++ side of sar-web's
// src/core/readers.ts).  A read past the end throws DecodeError; nothing here
// ever touches memory outside the buffer it was given.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace p2demo {
namespace detail {

// Anything the input makes impossible: a read past the end, a value out of
// range, a structure too large to be real.  Caught inside Parse(); it never
// crosses the public API.
class DecodeError : public std::runtime_error {
public:
	explicit DecodeError(const std::string &what) : std::runtime_error(what) {}
	explicit DecodeError(const char *what) : std::runtime_error(what) {}
};

struct ByteSpan {
	const uint8_t *data = nullptr;
	size_t size = 0;
};

class ByteReader {
public:
	ByteReader(const uint8_t *data, size_t size, size_t pos = 0) : data_(data), size_(size), pos_(pos < size ? pos : size) {}

	size_t pos() const { return pos_; }
	size_t remaining() const { return size_ - pos_; }

	uint8_t u8() {
		need(1);
		return data_[pos_++];
	}
	uint16_t u16() {
		need(2);
		const uint16_t v = uint16_t(data_[pos_] | (data_[pos_ + 1] << 8));
		pos_ += 2;
		return v;
	}
	uint32_t u32() {
		need(4);
		const uint8_t *p = data_ + pos_;
		pos_ += 4;
		return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
	}
	int32_t i32() { return int32_t(u32()); }
	float f32() {
		const uint32_t bits = u32();
		float v;
		std::memcpy(&v, &bits, sizeof v);
		return v;
	}

	ByteSpan bytes(size_t n) {
		need(n);
		const ByteSpan span{data_ + pos_, n};
		pos_ += n;
		return span;
	}
	void skip(size_t n) {
		need(n);
		pos_ += n;
	}

	// NUL-terminated; a string the data ends in the middle of is taken as far
	// as it goes (and is empty at the very end), as the TypeScript reader does
	std::string cstring() {
		size_t end = pos_;
		while (end < size_ && data_[end] != 0) end++;
		std::string s(reinterpret_cast<const char *>(data_ + pos_), end - pos_);
		pos_ = end < size_ ? end + 1 : size_;
		return s;
	}

	// fixed-size, NUL-padded
	std::string fixedString(size_t n) {
		const ByteSpan raw = bytes(n);
		size_t len = 0;
		while (len < n && raw.data[len] != 0) len++;
		return std::string(reinterpret_cast<const char *>(raw.data), len);
	}

private:
	void need(size_t n) const {
		if (n > size_ - pos_) throw DecodeError("read past the end of the data");
	}

	const uint8_t *data_;
	size_t size_;
	size_t pos_;
};

// Source's bf_read: bits are taken from the lowest bit of each byte up.
// Positions are size_t bit counts (32-bit arithmetic in the 32-bit plugin:
// this is the decoder's inner loop), so a buffer may hold at most SIZE_MAX
// bits; larger ones are refused.
class BitReader {
public:
	BitReader() = default;
	BitReader(const uint8_t *data, size_t byteCount) : data_(data), end_(byteCount * 8) {
		if (byteCount > SIZE_MAX / 8) throw DecodeError("bit buffer too large");
	}
	explicit BitReader(ByteSpan span) : BitReader(span.data, span.size) {}

	size_t pos() const { return pos_; }
	size_t remaining() const { return end_ - pos_; }

	bool bit() {
		if (pos_ >= end_) pastEnd();
		const bool b = ((data_[pos_ >> 3] >> (pos_ & 7)) & 1) != 0;
		pos_++;
		return b;
	}

	// 0..32 bits, unsigned
	uint32_t u(int n) {
		if (n <= 0) {
			if (n < 0) throw DecodeError("negative bit count");
			return 0;
		}
		if (n > 32) throw DecodeError("bit field wider than 32 bits");
		if (size_t(n) > end_ - pos_) pastEnd();
		// the field spans 1..5 bytes, all inside the buffer (checked above)
		const uint8_t *p = data_ + (pos_ >> 3);
		const unsigned shift = unsigned(pos_ & 7);
		const unsigned span = shift + unsigned(n);
		uint32_t low = p[0];
		if (span > 8) low |= uint32_t(p[1]) << 8;
		if (span > 16) low |= uint32_t(p[2]) << 16;
		if (span > 24) low |= uint32_t(p[3]) << 24;
		uint32_t v = low >> shift;
		if (span > 32) v |= uint32_t(p[4]) << (32 - shift);  // (shift is 1..7 here)
		if (n < 32) v &= (uint32_t(1) << n) - 1;
		pos_ += size_t(n);
		return v;
	}

	// 0..32 bits, two's complement
	int32_t s(int n) {
		const uint32_t v = u(n);
		if (n <= 0 || n >= 32) return int32_t(v);
		return (v >> (n - 1)) ? int32_t(int64_t(v) - (int64_t(1) << n)) : int32_t(v);
	}

	float f32() {
		const uint32_t bits = u(32);
		float v;
		std::memcpy(&v, &bits, sizeof v);
		return v;
	}

	// A bit field of any width as a number.  Send props say how wide their
	// values are (7 bits: up to 127); real ones stop at 32, wider ones are
	// summed the way the TypeScript reader does (in a double).
	double number(int n, bool isSigned) {
		if (n <= 32) return isSigned ? double(s(n)) : double(u(n));
		if (size_t(n) > end_ - pos_) pastEnd();
		double v = 0, weight = 1;
		for (int i = 0; i < n; i++, weight *= 2) {
			const size_t at = pos_ + size_t(i);
			if ((data_[at >> 3] >> (at & 7)) & 1) v += weight;
		}
		pos_ += size_t(n);
		// weight is now 2^n
		return isSigned && v >= weight / 2 ? v - weight : v;
	}

	void skip(uint64_t bits) {
		if (bits > end_ - pos_) pastEnd();
		pos_ += size_t(bits);
	}

	// Skips, stopping at the end: only a later read fails.  (Where the
	// reference moves its position without looking.)
	void skipToEndAtMost(uint64_t bits) { pos_ = bits > end_ - pos_ ? end_ : pos_ + size_t(bits); }

	// A reader over the next `bits` bits (no copy); this one moves past them.
	BitReader sub(uint64_t bits) {
		if (bits > end_ - pos_) throw DecodeError("message runs past the end of the packet");
		BitReader r;
		r.data_ = data_;
		r.pos_ = pos_;
		r.end_ = pos_ + size_t(bits);
		pos_ += size_t(bits);
		return r;
	}

	// 8-bit characters up to a NUL
	std::string cstring() {
		std::string s;
		for (;;) {
			const char c = char(u(8));
			if (c == 0) return s;
			s.push_back(c);
		}
	}
	void skipCString() {
		while (u(8) != 0) {}
	}

private:
	[[noreturn]] static void pastEnd() { throw DecodeError("read past the end of the bit buffer"); }

	const uint8_t *data_ = nullptr;
	size_t pos_ = 0;
	size_t end_ = 0;
};

}  // namespace detail
}  // namespace p2demo
