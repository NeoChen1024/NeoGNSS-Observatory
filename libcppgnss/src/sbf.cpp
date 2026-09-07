// SPDX-License-Identifier: GPL-3.0-only
#include <bit>
#include <cppgnss/sbf.hpp>
#include <cstdio>
#include <stdexcept>

namespace cppgnss::SBF {
namespace {
struct Decoder {
    std::span<const uint8_t> bytes;
    Fields fields;
    size_t offset = 0;
    std::vector<size_t> indices;
    std::string suffix(size_t depth) const {
        std::string s;
        for (size_t i = 0; i < depth; ++i) {
            char b[32];
            std::snprintf(b, sizeof b, "_%02zu", indices.at(i));
            s += b;
        }
        return s;
    }
    uint64_t reference(const std::string &ref) const {
        const auto split = ref.find('+');
        const auto name =
            ref.substr(0, split) + (split == std::string::npos ? "" : suffix(std::stoul(ref.substr(split + 1))));
        auto it = fields.find(name);
        if (it == fields.end())
            throw std::runtime_error("Missing SBF count/condition field: " + name);
        if (auto p = std::get_if<uint64_t>(&it->second))
            return *p;
        if (auto p = std::get_if<int64_t>(&it->second); p && *p >= 0)
            return *p;
        throw std::runtime_error("Noninteger SBF count/condition: " + name);
    }
    std::span<const uint8_t> take(size_t n) {
        if (n > bytes.size() - offset)
            throw std::runtime_error("SBF field exceeds payload at " + std::to_string(offset));
        auto value = bytes.subspan(offset, n);
        offset += n;
        return value;
    }
    static uint64_t uint(std::span<const uint8_t> b) {
        if (b.size() > 8)
            throw std::runtime_error("SBF integer wider than 64 bits");
        uint64_t n = 0;
        for (size_t i = 0; i < b.size(); ++i)
            n |= uint64_t(b[i]) << (8 * i);
        return n;
    }
    void sequence(const std::vector<Field> &specs, size_t base) {
        for (const auto &f : specs) {
            auto name = f.name + suffix(indices.size());
            switch (f.kind) {
            case Kind::padding: {
                const auto length = reference(f.reference);
                if (offset - base > length)
                    throw std::runtime_error("SBF sub-block shorter than its defined fields");
                take(length - (offset - base));
                break;
            }
            case Kind::optional: {
                const auto v = reference(f.reference);
                for (auto wanted : f.condition)
                    if (v == uint64_t(wanted)) {
                        sequence(f.children, base);
                        break;
                    }
                break;
            }
            case Kind::repeat: {
                size_t count = f.reference.empty() ? f.count : reference(f.reference);
                if (f.reference == "RLMLength")
                    count = count == 160 ? 5 : 3;
                if (count > bytes.size())
                    throw std::runtime_error("SBF repetition count exceeds payload");
                indices.push_back(0);
                for (size_t i = 0; i < count; ++i) {
                    indices.back() = i + 1;
                    sequence(f.children, offset);
                }
                indices.pop_back();
                break;
            }
            case Kind::bits: {
                auto data = take(f.width);
                size_t bit = 0;
                for (const auto &member : f.children) {
                    if (member.width > 64 || bit + member.width > data.size() * 8)
                        throw std::runtime_error("Invalid SBF bit schema");
                    uint64_t value = 0;
                    for (size_t j = 0; j < member.width; ++j)
                        value |= uint64_t((data[(bit + j) / 8] >> ((bit + j) % 8)) & 1) << j;
                    fields[member.name + suffix(indices.size())] = value;
                    bit += member.width;
                }
                break;
            }
            case Kind::scalar: {
                auto data = take(f.type == 'V' ? bytes.size() - offset : f.width);
                if (f.type == 'P')
                    break;
                if (f.type == 'X' || f.type == 'C' || f.type == 'V')
                    fields[name] = std::vector<uint8_t>(data.begin(), data.end());
                else if (f.type == 'F') {
                    auto bits = uint(data);
                    if (data.size() != 4 && data.size() != 8)
                        throw std::runtime_error("Invalid SBF floating-point width");
                    fields[name] = (data.size() == 4 ? double(std::bit_cast<float>(uint32_t(bits)))
                                                     : std::bit_cast<double>(bits)) *
                                   f.scale;
                } else {
                    if (data.size() > 8) {
                        if (f.scale != 1)
                            throw std::runtime_error("Scaled wide SBF integer is unsupported");
                        fields[name] = WideInteger{{data.begin(), data.end()}, f.type == 'I'};
                        break;
                    }
                    auto value = uint(data);
                    if (f.type == 'I') {
                        if (data.size() < 8 && !data.empty() && (data.back() & 128))
                            value |= ~uint64_t{0} << (8 * data.size());
                        auto n = std::bit_cast<int64_t>(value);
                        if (f.scale == 1)
                            fields[name] = n;
                        else
                            fields[name] = double(n) * f.scale;
                    } else if (f.scale == 1)
                        fields[name] = value;
                    else
                        fields[name] = double(value) * f.scale;
                }
                break;
            }
            }
        }
    }
};
} // namespace
Block decode(uint16_t id, uint8_t revision, std::span<const uint8_t> payload) {
    Block out{id, revision, "", Status::unknown_block, {}, {payload.begin(), payload.end()}, {}, "", std::nullopt};
    for (const auto &schema : schemas())
        if (schema.id == id) {
            out.name = schema.name;
            if (schema.fields.empty()) {
                out.status = Status::unsupported_schema;
                return out;
            }
            Decoder d{payload, {}, 0, {}};
            try {
                d.sequence(schema.fields, 0);
                out.status = Status::decoded;
            } catch (const std::exception &e) {
                out.status = Status::invalid_payload;
                out.error = e.what();
            }
            out.fields = std::move(d.fields);
            out.trailing.assign(payload.begin() + d.offset, payload.end());
            return out;
        }
    return out;
}
std::optional<SbasFrame> extract_sbas_l1(const Block &block) {
    if (block.id != 4020 || block.status != Status::decoded)
        return std::nullopt;
    const auto number = [&](const char *name) { return std::get<uint64_t>(block.fields.at(name)); };
    const auto svid = number("SVID"), signal = number("SigIdx");
    if (signal != 24 || !((svid >= 120 && svid <= 140) || (svid >= 198 && svid <= 215)))
        return std::nullopt;
    const auto &wire = std::get<std::vector<uint8_t>>(block.fields.at("NavBits"));
    if (wire.size() != 32)
        return std::nullopt;
    std::array<uint8_t, 32> bits;
    // SBF stores eight little-endian U4 words containing MSB-first air bits.
    for (size_t i = 0; i < 8; ++i)
        for (size_t j = 0; j < 4; ++j)
            bits[4 * i + j] = wire[4 * i + 3 - j];
    return SbasFrame{uint32_t(number("TOW")),
                     uint16_t(number("WNc")),
                     uint16_t(svid <= 140 ? svid : svid - 57),
                     uint8_t(svid),
                     uint8_t(signal),
                     uint8_t(number("FreqNr")),
                     uint8_t(number("RxChannel")),
                     uint8_t(number("ViterbiCnt")),
                     number("CRCPassed") != 0,
                     cppgnss::SBAS::parse_l1(bits)};
}
} // namespace cppgnss::SBF
