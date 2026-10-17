#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rawrcam::video {

// Classifies encoded HEVC access units as I, P or B from the first slice header,
// so stats report what the encoder produced rather than what was requested.
class HevcFrameTypeParser final {
public:
    enum class Type { Unknown, I, P, B };

    // Reads num_extra_slice_header_bits from the PPS in csd-0 (Annex B).
    void setCodecConfig(const uint8_t* data, size_t size) noexcept {
        forEachNal(data, size, [this](const uint8_t* nal, size_t length) {
            if (length < 3 || nalType(nal) != 34) return;
            BitReader bits(unescape(nal + 2, length - 2));
            bits.ue();  // pps_pic_parameter_set_id
            bits.ue();  // pps_seq_parameter_set_id
            bits.u(1);  // dependent_slice_segments_enabled_flag
            bits.u(1);  // output_flag_present_flag
            extraSliceHeaderBits_ = static_cast<int>(bits.u(3));
        });
    }

    [[nodiscard]] Type classify(const uint8_t* data, size_t size) const noexcept {
        Type type = Type::Unknown;
        forEachNal(data, size, [&](const uint8_t* nal, size_t length) {
            if (type != Type::Unknown || length < 3) return;
            const int kind = nalType(nal);
            if (kind > 21) return;  // Not a VCL slice.
            BitReader bits(unescape(nal + 2, std::min<size_t>(length - 2, 32)));
            if (bits.u(1) == 0) return;  // Only the first slice segment carries a full header we can read.
            if (kind >= 16) bits.u(1);  // no_output_of_prior_pics_flag (IRAP)
            bits.ue();                  // slice_pic_parameter_set_id
            bits.u(extraSliceHeaderBits_);
            switch (bits.ue()) {
                case 0: type = Type::B; break;
                case 1: type = Type::P; break;
                case 2: type = Type::I; break;
                default: break;
            }
        });
        return type;
    }

private:
    class BitReader {
    public:
        explicit BitReader(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
        uint32_t u(int count) noexcept {
            uint32_t value = 0;
            for (int i = 0; i < count; ++i) {
                const size_t byte = position_ >> 3;
                const uint32_t bit = byte < bytes_.size() ? (bytes_[byte] >> (7 - (position_ & 7))) & 1u : 0u;
                value = (value << 1) | bit;
                ++position_;
            }
            return value;
        }
        uint32_t ue() noexcept {
            int zeros = 0;
            while (u(1) == 0 && zeros < 31) ++zeros;
            return (1u << zeros) - 1u + u(zeros);
        }

    private:
        std::vector<uint8_t> bytes_;
        size_t position_ = 0;
    };

    static int nalType(const uint8_t* nal) noexcept { return (nal[0] >> 1) & 0x3f; }

    static std::vector<uint8_t> unescape(const uint8_t* data, size_t size) {
        std::vector<uint8_t> out;
        out.reserve(size);
        int zeros = 0;
        for (size_t i = 0; i < size; ++i) {
            if (zeros >= 2 && data[i] == 3) {
                zeros = 0;
                continue;
            }
            zeros = data[i] == 0 ? zeros + 1 : 0;
            out.push_back(data[i]);
        }
        return out;
    }

    template <typename Visit>
    static void forEachNal(const uint8_t* data, size_t size, Visit&& visit) {
        size_t start = SIZE_MAX;
        for (size_t i = 0; i + 2 < size; ++i) {
            if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) continue;
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && data[end - 1] == 0) --end;
                visit(data + start, end - start);
            }
            start = i + 3;
            i += 2;
        }
        if (start != SIZE_MAX && start < size) visit(data + start, size - start);
    }

    int extraSliceHeaderBits_ = 0;
};

}  // namespace rawrcam::video
