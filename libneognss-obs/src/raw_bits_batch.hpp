// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "arrow_batch.hpp"
#include <algorithm>
#include <neognss_obs/raw_bits.hpp>
#include <span>
#include <vector>

namespace neognss_obs {
// Canonical RawBits rows of one Arrow batch (docs/commonnex/raw-bits.md).
// Columns are resolved and their storage types checked once per batch. Row
// accessors take 0-based batch indices and check the row against the shared
// layout registry; consumers keep only their family selection and policy.
class RawBitsBatch {
    ArrowBatchView owner_;
    Column root_, setup_, family_, time_, system_, number_, format_, content_,
        length_, completeness_, checks_, check_, result_, sources_, source_,
        body_;
    int64_t row(int64_t i) const { return i + owner_.value.offset; }
    // [begin, end) child indices of list column LIST at batch index I.
    static std::pair<int64_t, int64_t> range(const Column &list, int64_t row) {
        return {
            ArrowArrayViewListChildOffset(list.v, row + list.v->offset),
            ArrowArrayViewListChildOffset(list.v, row + list.v->offset + 1)};
    }

  public:
    struct Identity {
        const RawBitsLayout &layout;
        std::string_view system;
        int64_t satellite;
    };
    RawBitsBatch(ArrowSchema *schema, ArrowArray *array)
        : owner_(schema, array), root_{&owner_.value, schema},
          setup_(root_.child("setup_id")),
          family_(root_.child("message_family")),
          time_(root_.child("nav_epoch_gpst")),
          system_(root_.child("satellite_system")),
          number_(root_.child("satellite_number")),
          format_(root_.child("body_format")),
          content_(root_.child("content_kind")),
          length_(root_.child("bit_length")),
          completeness_(root_.child("completeness")),
          checks_(root_.child("checks")),
          sources_(root_.child("bitstream_source")),
          body_(root_.child("body")) {
        if (!time_.decimal_seconds() ||
            number_.v->storage_type != NANOARROW_TYPE_UINT16 ||
            length_.v->storage_type != NANOARROW_TYPE_UINT32 ||
            body_.v->storage_type != NANOARROW_TYPE_BINARY ||
            checks_.v->storage_type != NANOARROW_TYPE_LIST ||
            sources_.v->storage_type != NANOARROW_TYPE_LIST ||
            checks_.v->children[0]->storage_type != NANOARROW_TYPE_STRUCT ||
            sources_.v->children[0]->storage_type != NANOARROW_TYPE_STRING)
            throw std::runtime_error("Invalid RawBits column types");
        check_ = {checks_.v->children[0], checks_.s->children[0]};
        result_ = check_.child("result");
        source_ = {sources_.v->children[0], sources_.s->children[0]};
    }
    int64_t size() const { return owner_.value.length; }
    std::string_view setup(int64_t i) const { return setup_.text(row(i)); }
    std::string_view family(int64_t i) const { return family_.text(row(i)); }
    std::optional<GpstTicks> time(int64_t i) const {
        return time_.seconds_ps(row(i));
    }
    // The broadcaster identity of a complete row whose system, format,
    // content kind and bit length match its family's registered layout.
    Identity identity(int64_t i) const {
        const auto *layout = raw_bits_layout(family(i));
        const auto system = system_.text(row(i));
        const auto satellite = number_.integer(row(i));
        if (!layout || system != layout->system || satellite < 1 ||
            satellite > 255 || format_.text(row(i)) != layout->format ||
            content_.text(row(i)) != layout->content() ||
            length_.integer(row(i)) != layout->bit_length ||
            completeness_.text(row(i)) != "complete")
            throw std::runtime_error("Invalid RawBits identity/layout");
        return {*layout, system, satellite};
    }
    // Processor acceptance: at least one passed and no failed check.
    bool accepted(int64_t i) const {
        bool pass = false, fail = false;
        if (!checks_.null(row(i)))
            for (auto [j, end] = range(checks_, row(i)); j < end; ++j) {
                const auto r = result_.text(j + check_.v->offset);
                pass |= r == "pass";
                fail |= r == "fail";
            }
        return pass && !fail;
    }
    // Known contributing signals in stored order. Consumers need at least one.
    std::vector<std::string> signals(int64_t i) const {
        if (sources_.null(row(i)))
            throw std::runtime_error("Null RawBits bitstream source");
        std::vector<std::string> out;
        for (auto [j, end] = range(sources_, row(i)); j < end; ++j)
            out.push_back(source_.str(j));
        if (out.empty())
            throw std::runtime_error("Empty RawBits bitstream source");
        return out;
    }
    // The body of BIT_LENGTH meaningful bits: ceil(bit_length / 8) bytes with
    // zero unused low bits in the last byte.
    std::span<const uint8_t> body(int64_t i, uint32_t bit_length) const {
        if (body_.null(row(i)))
            throw std::runtime_error("Null RawBits body");
        const auto bytes = ArrowArrayViewGetBytesUnsafe(body_.v, row(i));
        const std::span<const uint8_t> data(bytes.data.as_uint8,
                                            size_t(bytes.size_bytes));
        if (data.size() != (bit_length + 7) / 8 ||
            (bit_length % 8 &&
             (data.back() & ((1u << (8 - bit_length % 8)) - 1))))
            throw std::runtime_error("Invalid RawBits body size/padding");
        return data;
    }
};
// "/A/B" over the sorted contributors: one source key for any stored order.
inline std::string contributor_key(std::vector<std::string> signals) {
    std::sort(signals.begin(), signals.end());
    std::string key;
    for (const auto &signal : signals)
        key += "/" + signal;
    return key;
}
} // namespace neognss_obs
