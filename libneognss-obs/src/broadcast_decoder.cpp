// SPDX-License-Identifier: GPL-3.0-only
#include "arrow_batch.hpp"
#include <algorithm>
#include <array>
#include <boost/int128/int128.hpp>
#include <cmath>
#include <mutex>
#include <neognss_obs/broadcast_decoder.hpp>
#include <numbers>
#include <optional>
#include <set>
#include <variant>
#include <vector>

namespace neognss_obs {
namespace broadcast_detail {
using Tick = boost::int128::int128;
constexpr int64_t ps = 1000000000000;
using Bytes = std::vector<uint8_t>;
using Value =
    std::variant<std::monostate, int64_t, double, std::string, bool, Tick,
                 Bytes, std::vector<int64_t>, std::vector<std::string>>;
using Row = std::map<std::string, Value>;
struct Column {
    ArrowArrayView *v;
    const ArrowSchema *s;
    Column child(const char *name) const {
        for (int64_t i = 0; i < s->n_children; ++i)
            if (s->children[i]->name &&
                std::string_view(s->children[i]->name) == name)
                return {v->children[i], s->children[i]};
        throw std::runtime_error(std::string("Missing RawBits field: ") + name);
    }
    bool null(int64_t i) const { return ArrowArrayViewIsNull(v, i); }
    std::string str(int64_t i) const {
        if (null(i) || v->storage_type != NANOARROW_TYPE_STRING)
            throw std::runtime_error("Expected string");
        auto x = ArrowArrayViewGetStringUnsafe(v, i);
        return {x.data, size_t(x.size_bytes)};
    }
    int64_t integer(int64_t i) const {
        if (null(i) || (v->storage_type != NANOARROW_TYPE_UINT16 &&
                        v->storage_type != NANOARROW_TYPE_UINT32))
            throw std::runtime_error("Invalid RawBits integer");
        return int64_t(ArrowArrayViewGetUIntUnsafe(v, i));
    }
    std::optional<Tick> time(int64_t i) const {
        if (std::string_view(s->format) != "d:38,12" &&
            std::string_view(s->format) != "d:38,12,128")
            throw std::runtime_error("Expected GPST decimal128(38,12)");
        if (null(i))
            return {};
        ArrowDecimal d;
        ArrowDecimalInit(&d, 128, 38, 12);
        ArrowArrayViewGetDecimalUnsafe(v, i, &d);
        if (d.words[d.high_word_index] >> 63)
            throw std::runtime_error("Negative GPST");
        Tick t = (Tick(d.words[d.high_word_index]) << 64) +
                 d.words[d.low_word_index];
        if (t / ps > INT64_MAX)
            throw std::runtime_error("GPST out of range");
        return t;
    }
};
void ok(int e) {
    if (e)
        throw std::runtime_error("Broadcast Arrow allocation/append failed");
}
ArrowType type(const Value &v) {
    switch (v.index()) {
    case 1:
        return NANOARROW_TYPE_INT64;
    case 2:
        return NANOARROW_TYPE_DOUBLE;
    case 3:
        return NANOARROW_TYPE_STRING;
    case 4:
        return NANOARROW_TYPE_BOOL;
    case 5:
        return NANOARROW_TYPE_DECIMAL128;
    case 6:
        return NANOARROW_TYPE_BINARY;
    case 7:
    case 8:
        return NANOARROW_TYPE_LIST;
    default:
        return NANOARROW_TYPE_DECIMAL128;
    }
}
void append(ArrowArray *a, const Value &v) {
    std::visit(
        [&](const auto &x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                ok(ArrowArrayAppendNull(a, 1));
            else if constexpr (std::is_same_v<T, int64_t> ||
                               std::is_same_v<T, bool>)
                ok(ArrowArrayAppendInt(a, x));
            else if constexpr (std::is_same_v<T, double>)
                ok(ArrowArrayAppendDouble(a, x));
            else if constexpr (std::is_same_v<T, std::string>)
                ok(ArrowArrayAppendString(a, {x.data(), int64_t(x.size())}));
            else if constexpr (std::is_same_v<T, Tick>) {
                ArrowDecimal d;
                ArrowDecimalInit(&d, 128, 38, 12);
                d.words[d.low_word_index] = uint64_t(x);
                d.words[d.high_word_index] = uint64_t(x >> 64);
                ok(ArrowArrayAppendDecimal(a, &d));
            } else if constexpr (std::is_same_v<T, Bytes>) {
                ArrowBufferView b{};
                b.data.as_uint8 = x.data();
                b.size_bytes = int64_t(x.size());
                ok(ArrowArrayAppendBytes(a, b));
            } else {
                for (const auto &entry : x)
                    append(a->children[0], Value(entry));
                ok(ArrowArrayFinishElement(a));
            }
        },
        v);
}
std::shared_ptr<CnexBatch> batch(const std::vector<Row> &rows) {
    auto out = std::make_shared<CnexBatch>();
    ArrowSchemaInit(&out->schema);
    ok(ArrowSchemaSetTypeStruct(&out->schema, int64_t(rows.front().size())));
    int64_t index = 0;
    for (const auto &[name, unused] : rows.front()) {
        Value sample;
        for (const auto &r : rows) {
            if (!std::holds_alternative<std::monostate>(r.at(name))) {
                sample = r.at(name);
                break;
            }
        }
        auto s = out->schema.children[index++];
        auto t = type(sample);
        if (std::holds_alternative<std::monostate>(sample) &&
            (name == "eccentricity" || name == "i0_rad"))
            t = NANOARROW_TYPE_DOUBLE;
        if (std::holds_alternative<std::monostate>(sample) &&
            name == "ephemeris_status_flag")
            t = NANOARROW_TYPE_BOOL;
        if (t == NANOARROW_TYPE_DECIMAL128)
            ok(ArrowSchemaSetTypeDecimal(s, t, 38, 12));
        else
            ok(ArrowSchemaSetType(s, t));
        if (t == NANOARROW_TYPE_LIST)
            ok(ArrowSchemaSetType(s->children[0], sample.index() == 8
                                                      ? NANOARROW_TYPE_STRING
                                                      : NANOARROW_TYPE_INT64));
        ok(ArrowSchemaSetName(s, name.c_str()));
    }
    ok(ArrowArrayInitFromSchema(&out->array, &out->schema, nullptr));
    ok(ArrowArrayStartAppending(&out->array));
    for (const auto &r : rows) {
        index = 0;
        for (const auto &[name, v] : r)
            append(out->array.children[index++], v);
        ok(ArrowArrayFinishElement(&out->array));
    }
    ArrowError e{};
    ok(ArrowArrayFinishBuildingDefault(&out->array, &e));
    return out;
}
struct Bits {
    std::array<uint8_t, 30> data{};
    uint64_t u(int start, int n) const {
        uint64_t x = 0;
        for (int k = 0; k < n; ++k)
            x = (x << 1) |
                ((data[(start + k) / 8] >> (7 - (start + k) % 8)) & 1);
        return x;
    }
    int64_t s(int start, int n) const {
        auto x = u(start, n);
        return (x & (uint64_t(1) << (n - 1)))
                   ? int64_t(x) - int64_t(uint64_t(1) << n)
                   : int64_t(x);
    }
    double scaled(int start, int n, int exp, bool sign = true,
                  double factor = 1) const {
        return std::ldexp(sign ? double(s(start, n)) : double(u(start, n)),
                          exp) *
               factor;
    }
};
Tick seconds(int64_t s) { return Tick(s) * ps; }
Tick duration(double s) { return Tick(std::nearbyint(s * double(ps))); }
Value tv(std::optional<Tick> t) { return t ? Value(*t) : Value{}; }
int64_t integer(const Row &r, const char *k) {
    return std::get<int64_t>(r.at(k));
}
// Resolve truncated weeks against reception context, never the host clock.
Tick reference(Tick now, int64_t week, int modulo, int64_t tow) {
    auto w = int64_t(now / seconds(604800));
    auto full = w + (week - w % modulo);
    if (full - w > modulo / 2)
        full -= modulo;
    if (w - full > modulo / 2)
        full += modulo;
    return seconds(full * 604800 + tow);
}
} // namespace broadcast_detail
using namespace broadcast_detail;

struct BroadcastMessageDecoder::State {
    struct Assembly {
        std::map<int, Row> parts;
        std::optional<Tick> start;
    };
    struct Candidate {
        std::string kind, key;
        Row row;
        Tick received, first;
    };
    struct AlmanacAssembly {
        std::optional<Tick> start;
        std::map<int, Row> slots;
        std::map<int, Row> values;
        std::optional<Tick> toa;
    };
    std::string setup;
    Tick interval;
    std::optional<Tick> progress, next;
    std::map<std::string, Assembly> ephemeris;
    std::map<std::string, AlmanacAssembly> almanacs;
    std::vector<Candidate> cache;
    std::map<std::string, uint64_t> counts;
    std::map<std::string, std::vector<Row>> output;
    mutable std::mutex mutex;
    bool failed = false;
    State(std::string id, int64_t period)
        : setup(std::move(id)), interval(seconds(period)) {
        if (period <= 0)
            throw std::invalid_argument("Snapshot interval must be positive");
    }
    void emit(const std::string &kind, Row r) {
        if (++counts["output_in_call"] > 100000)
            throw std::runtime_error(
                "Broadcast output limit exceeded; feed smaller batches");
        r["output_sequence"] = int64_t(counts["output_records"]++);
        output[kind].push_back(std::move(r));
    }
    void collect_almanac(const Bits &b, const Row &base, const Row &fields,
                         const std::string &key, std::optional<Tick> t) {
        if (!t || std::get<bool>(base.at("alert")))
            return;
        const int sf = int(b.u(43, 3)), id = int(b.u(50, 6));
        // GPS section-20 nominal page schedule, confirmed against HOW. Dummy
        // pages have no subject ID, so only a valid HOW can locate their slot.
        auto tow = b.u(24, 17) * 6;
        if (tow >= 604800 || tow % 30 != uint64_t(sf * 6 % 30)) {
            ++counts["almanac_schedule_mismatch"];
            return;
        }
        int page = int(((tow + 604800 - 6) % 604800) / 30 % 25) + 1;
        int slot = 0;
        if (sf == 5 && page <= 24)
            slot = page;
        else if (sf == 4 && page >= 2 && page <= 5)
            slot = page + 23;
        else if (sf == 4 && page >= 7 && page <= 10)
            slot = page + 22;
        else if (sf == 5 && page == 25)
            slot = 51;
        else if (sf == 4 && page == 25)
            slot = 63;
        if (!slot)
            return;
        if (id != slot && !(id == 0 && slot <= 32)) {
            ++counts["almanac_schedule_mismatch"];
            return;
        }
        auto &a = almanacs[key];
        if (a.start && *t - *a.start > seconds(2250)) {
            a = {};
            ++counts["almanac_timeout"];
        }
        std::optional<Tick> toa;
        if (fields.contains("toa_s"))
            toa = std::get<Tick>(fields.at("toa_s"));
        Row content = fields;
        content.erase("reference_gpst");
        if ((toa && a.toa && *toa != *a.toa) ||
            (a.slots.contains(slot) && a.slots.at(slot) != content)) {
            a = {};
            ++counts["almanac_changed"];
        }
        if (!a.start)
            a.start = t;
        if (toa)
            a.toa = toa;
        a.slots[slot] = content;
        Row value = base;
        value.insert(fields.begin(), fields.end());
        a.values[slot] = value;
        if (a.slots.size() != 34 || !a.slots.contains(51) ||
            !a.slots.contains(63))
            return;
        const auto &epoch = a.values.at(51);
        if (!a.toa || !epoch.contains("reference_gpst") ||
            std::holds_alternative<std::monostate>(epoch.at("reference_gpst")))
            return;
        int64_t count = 0;
        for (const auto &[n, v] : a.values)
            if (n <= 32 && v.contains("subject_sv_id")) {
                auto row = v;
                row["completed_gpst"] = *t;
                row["reference_gpst"] = epoch.at("reference_gpst");
                row["first_received_gpst"] = *a.start;
                emit("almanac_set_entries", std::move(row));
                ++count;
            }
        Row summary = base;
        summary["reference_gpst"] = epoch.at("reference_gpst");
        summary["first_received_gpst"] = *a.start;
        summary["received_satellite_count"] = count;
        summary["completeness"] = std::string("COMPLETE");
        emit("almanac_set", std::move(summary));
        ++counts["almanac_sets"];
        a = {};
    }
    void remember(const std::string &kind, const std::string &key, const Row &r,
                  std::optional<Tick> t) {
        if (!t)
            return;
        // Keep distinct parameter candidates; receipt headers are not content.
        auto content = [](Row x) {
            for (auto name : {"nav_epoch_gpst", "first_received_gpst",
                              "tow_count", "subframe_id", "alert", "antispoof"})
                x.erase(name);
            return x;
        };
        auto value = content(r);
        for (auto &c : cache)
            if (c.kind == kind && c.key == key && content(c.row) == value) {
                c.row = r;
                c.received = *t;
                return;
            }
        std::erase_if(cache, [&](const auto &c) {
            return *t - c.received > seconds(7 * 86400);
        });
        if (cache.size() >= 8192)
            throw std::runtime_error("Broadcast candidate capacity exceeded");
        cache.push_back({kind, key, r, *t, *t});
    }
    void advance(Tick t) {
        if (progress && t < *progress)
            throw std::runtime_error(
                "Broadcast navigation context reversed; declare discontinuity");
        if (!next)
            next = (t / interval + 1) * interval;
        while (*next <= t) {
            Row summary{
                {"snapshot_gpst", *next},
                {"setup_id", setup},
                {"statistics_scope", std::string("DECODER_LIFETIME")},
                {"decoded_messages", int64_t(counts["decoded_messages"])}};
            emit("snapshot", std::move(summary));
            for (const auto &c : cache) {
                if (c.received >= *next ||
                    *next - c.received > seconds(7 * 86400))
                    continue;
                Row r = c.row;
                r["snapshot_gpst"] = *next;
                r["first_received_gpst"] = c.first;
                r["applicability"] = std::string("UNKNOWN");
                if (c.kind == "almanac_entry") {
                    r["reference_gpst"] = Value{};
                    auto prefix = c.key.substr(0, c.key.rfind(':'));
                    for (const auto &epoch : cache)
                        if (epoch.kind == "almanac_epoch" &&
                            epoch.key == prefix + ":51" &&
                            epoch.row.at("toa_s") == r.at("toa_s") &&
                            epoch.received <= *next &&
                            !std::holds_alternative<std::monostate>(
                                epoch.row.at("reference_gpst"))) {
                            if (!std::holds_alternative<std::monostate>(
                                    r.at("reference_gpst")) &&
                                r.at("reference_gpst") !=
                                    epoch.row.at("reference_gpst")) {
                                r["reference_gpst"] = Value{};
                                break;
                            }
                            r["reference_gpst"] =
                                epoch.row.at("reference_gpst");
                        }
                }
                if (c.kind == "ephemeris") {
                    if (std::holds_alternative<std::monostate>(
                            r.at("toe_gpst")))
                        continue;
                    auto toe = std::get<Tick>(r.at("toe_gpst"));
                    const bool q =
                        std::get<std::string>(r.at("satellite_system")) == "J";
                    auto iodc = integer(r, "iodc");
                    int hours = 0;
                    if (!std::get<bool>(r.at("fit_interval_flag")))
                        hours = q ? 2 : 4;
                    else if (!q) {
                        if ((iodc & 255) < 240)
                            hours = 6;
                        else if (iodc >= 240 && iodc <= 247)
                            hours = 8;
                        else if ((iodc >= 248 && iodc <= 255) || iodc == 496)
                            hours = 14;
                        else if ((iodc >= 497 && iodc <= 503) ||
                                 (iodc >= 1021 && iodc <= 1023))
                            hours = 26;
                    }
                    if (!hours)
                        continue; // No unsupported fit model advertised as
                                  // valid.
                    auto half = seconds(hours * 1800);
                    auto clock_half =
                        q ? seconds((15LL << (iodc >> 8)) * 30) : half;
                    auto toc = std::get<Tick>(r.at("toc_gpst"));
                    if (*next < toe - half || *next > toe + half ||
                        *next < toc - clock_half || *next > toc + clock_half)
                        continue;
                    r["applicability"] = std::string("ORBIT_CLOCK_FIT");
                }
                emit("snapshot_" + c.kind, std::move(r));
            }
            *next += interval;
        }
        progress = t;
    }
    void decode(const Bits &b, Row base, const std::string &key,
                std::optional<Tick> t) {
        const int sf = int(b.u(43, 3));
        const bool q =
            std::get<std::string>(base.at("satellite_system")) == "J";
        const double pi = std::numbers::pi;
        base["subframe_id"] = int64_t(sf);
        base["tow_count"] = int64_t(b.u(24, 17));
        base["alert"] = bool(b.u(41, 1));
        base["antispoof"] = bool(b.u(42, 1));
        if (b.u(0, 8) != 0x8b || sf < 1 || sf > 5) {
            ++counts["invalid_header"];
            return;
        }
        Row fields;
        auto u = [&](const char *n, int p, int len) {
            fields[n] = int64_t(b.u(p, len));
        };
        auto f = [&](const char *n, int p, int len, int exp, bool sign = true,
                     double factor = 1) {
            fields[n] = b.scaled(p, len, exp, sign, factor);
        };
        if (sf == 1) {
            u("week_raw", 48, 10);
            u("code_on_l2_raw", 58, 2);
            fields["ephemeris_status_flag"] =
                q ? Value(bool(b.u(59, 1))) : Value{};
            u("ura_index", 60, 4);
            u("health_raw", 64, 6);
            fields["iodc"] = int64_t((b.u(70, 2) << 8) | b.u(168, 8));
            fields["l2_p_data_flag"] = bool(b.u(72, 1));
            fields["tgd_s"] = b.s(160, 8) == -128
                                  ? Value{}
                                  : Value(duration(b.scaled(160, 8, -31)));
            fields["toc_s"] = seconds(int64_t(b.u(176, 16)) * 16);
            fields["af0_s"] = duration(b.scaled(216, 22, -31));
            f("af1_s_s", 200, 16, -43);
            f("af2_s_s2", 192, 8, -55);
            fields["tgd_reference"] =
                std::string(q ? "QZSS_L1_CA_OR_CB" : "GPS_L1_PY");
        } else if (sf == 2) {
            u("iode_sf2", 48, 8);
            f("crs_m", 56, 16, -5);
            f("delta_n_rad_s", 72, 16, -43, true, pi);
            f("m0_rad", 88, 32, -31, true, pi);
            f("cuc_rad", 120, 16, -29);
            f("eccentricity", 136, 32, -33, false);
            f("cus_rad", 168, 16, -29);
            f("sqrt_a", 184, 32, -19, false);
            fields["toe_s"] = seconds(int64_t(b.u(216, 16)) * 16);
            fields["fit_interval_flag"] = bool(b.u(232, 1));
            u("aodo_raw", 233, 5);
        } else if (sf == 3) {
            f("cic_rad", 48, 16, -29);
            f("omega0_rad", 64, 32, -31, true, pi);
            f("cis_rad", 96, 16, -29);
            f("i0_rad", 112, 32, -31, true, pi);
            f("crc_m", 144, 16, -5);
            f("omega_rad", 160, 32, -31, true, pi);
            f("omega_dot_rad_s", 192, 24, -43, true, pi);
            u("iode_sf3", 216, 8);
            f("idot_rad_s", 224, 14, -43, true, pi);
        }
        if (sf <= 3) {
            Row r = base;
            r.insert(fields.begin(), fields.end());
            emit("lnav_sf" + std::to_string(sf), r);
            ++counts["decoded_messages"];
            if (!t || std::get<bool>(base.at("alert")))
                return;
            auto &a = ephemeris[key];
            if (a.start && *t - *a.start > seconds(90)) {
                a = {};
                ++counts["assembly_timeout"];
            }
            const auto issue = integer(fields, sf == 1   ? "iodc"
                                               : sf == 2 ? "iode_sf2"
                                                         : "iode_sf3") &
                               255;
            for (const auto &[part, old] : a.parts) {
                auto other = integer(old, part == 1   ? "iodc"
                                          : part == 2 ? "iode_sf2"
                                                      : "iode_sf3") &
                             255;
                if (issue != other || (part == sf && old != fields)) {
                    a = {};
                    ++counts["assembly_changed"];
                    break;
                }
            }
            if (!a.start)
                a.start = t;
            a.parts[sf] = fields;
            if (a.parts.size() == 3) {
                Row full = base;
                full.erase("subframe_id");
                for (const auto &[part, v] : a.parts)
                    full.insert(v.begin(), v.end());
                full["first_received_gpst"] = *a.start;
                const auto week = integer(full, "week_raw");
                auto toe = int64_t(std::get<Tick>(full.at("toe_s")) / ps),
                     toc = int64_t(std::get<Tick>(full.at("toc_s")) / ps);
                auto wr = reference(*t, week, 1024, 0);
                auto resolve = [&](int64_t sec) {
                    auto x = wr + seconds(sec);
                    if (x - *t > seconds(302400))
                        x -= seconds(604800);
                    if (*t - x > seconds(302400))
                        x += seconds(604800);
                    return x;
                };
                full["toe_gpst"] = resolve(toe);
                full["toc_gpst"] = resolve(toc);
                if (toe >= 604800 || toc >= 604800 ||
                    std::get<double>(full.at("sqrt_a")) <= 0 ||
                    std::get<double>(full.at("eccentricity")) >= 1)
                    ++counts["invalid_ephemeris"];
                else {
                    emit("ephemeris", full);
                    remember("ephemeris", key, full, t);
                    ++counts["ephemerides"];
                }
                a = {};
            }
            return;
        }
        const int id = int(b.u(50, 6)), dataid = int(b.u(48, 2));
        if (dataid != (q ? 3 : 1)) {
            ++counts["unsupported_data_id"];
            return;
        }
        base["data_id_raw"] = int64_t(dataid);
        base["sv_id_raw"] = int64_t(id);
        if (id == 0) {
            ++counts[q ? "test_mode" : "dummy"];
            if (!q)
                collect_almanac(b, base, {}, key, t);
            return;
        }
        std::string kind;
        if ((!q &&
             ((sf == 5 && id <= 24) || (sf == 4 && id >= 25 && id <= 32))) ||
            (q && id <= 10)) {
            kind = "almanac_entry";
            u("subject_sv_id", 50, 6);
            // QPNT-006 known QZO slots 2-5; GEO/QGEO slots 7-9. Other slots
            // lack a defined reference orbit in this edition.
            bool mapped = !q || (id >= 2 && id <= 5) || (id >= 7 && id <= 9);
            fields["orbit_reference_known"] = mapped;
            fields["eccentricity"] = mapped
                                         ? Value(b.scaled(56, 16, -21, false) +
                                                 (q && id <= 5 ? 0.06 : 0))
                                         : Value{};
            fields["i0_rad"] = mapped
                                   ? Value((b.scaled(80, 16, -19) +
                                            (q ? (id <= 5 ? 0.25 : 0) : 0.3)) *
                                           pi)
                                   : Value{};
            fields["toa_s"] = seconds(int64_t(b.u(72, 8)) * 4096);
            f("omega_dot_rad_s", 96, 16, -38, true, pi);
            u("health_raw", 112, 8);
            f("sqrt_a", 120, 24, -11, false);
            f("omega0_rad", 144, 24, -23, true, pi);
            f("omega_rad", 168, 24, -23, true, pi);
            f("m0_rad", 192, 24, -23, true, pi);
            fields["af0_s"] =
                duration(double(b.s(216, 8)) * std::ldexp(1., -20) +
                         double(b.u(235, 3)) * std::ldexp(1., -17));
            f("af1_s_s", 224, 11, -38);
        } else if (id == 51 && (q || sf == 5)) {
            kind = "almanac_epoch";
            fields["toa_s"] = seconds(int64_t(b.u(56, 8)) * 4096);
            u("week_raw", 64, 8);
            std::vector<int64_t> h;
            for (int i = 0; i < (q ? 10 : 24); ++i)
                h.push_back(int64_t(b.u(72 + i * 6, 6)));
            fields["health_by_slot"] = h;
            fields["reference_gpst"] =
                t ? Value(reference(*t, int64_t(b.u(64, 8)), 256,
                                    int64_t(b.u(56, 8)) * 4096))
                  : Value{};
        } else if (id == 63 && !q && sf == 4) {
            kind = "configuration_health";
            std::vector<int64_t> config, health;
            for (int i = 0; i < 32; ++i)
                config.push_back(int64_t(b.u(56 + 4 * i, 4)));
            for (int i = 0; i < 8; ++i)
                health.push_back(int64_t(b.u(186 + 6 * i, 6)));
            fields["configuration_by_slot"] = config;
            fields["health_slots_25_32"] = health;
        } else if (id == 55 && (q || sf == 4)) {
            kind = "special_message";
            Bytes bytes(b.data.begin() + 7, b.data.begin() + 29);
            fields["payload"] = bytes;
            bool charset = true;
            std::string display;
            const char *hex = "0123456789ABCDEF";
            for (auto ch : bytes) {
                if (ch == 0xf8)
                    display += "\xc2\xb0";
                else if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                         std::string("+-.\x27/ :\"").find(char(ch)) !=
                             std::string::npos)
                    display += char(ch);
                else {
                    charset = false;
                    display += "\\x";
                    display += hex[ch >> 4];
                    display += hex[ch & 15];
                }
            }
            fields["display_text"] = display;
            fields["icd_character_set"] = charset;
        } else if (id == 52 && !q && sf == 4) {
            kind = "nmct";
            u("availability_indicator", 56, 2);
            Bytes payload(23, 0);
            for (int i = 0; i < 180; ++i)
                payload[i / 8] |= uint8_t(b.u(58 + i, 1) << (7 - i % 8));
            fields["erd_payload"] = payload;
            fields["erd_payload_bit_length"] = int64_t(180);
            std::vector<int64_t> codes;
            if (b.u(56, 2) == 0)
                for (int i = 0; i < 30; ++i)
                    codes.push_back(b.s(58 + 6 * i, 6));
            fields["unencrypted_erd_codes"] = codes;
        } else if (id == 60 && q) {
            kind = "qznma_payload";
            Bytes payload(23, 0);
            for (int i = 0; i < 182; ++i)
                payload[i / 8] |= uint8_t(b.u(56 + i, 1) << (7 - i % 8));
            fields["payload"] = payload;
            fields["payload_bit_length"] = int64_t(182);
        } else if ((id == 56 && (q || sf == 4)) || (q && id == 61)) {
            kind = "ionosphere_utc";
            fields["region"] =
                std::string(q ? (id == 61 ? "JAPAN" : "WIDE_AREA") : "GLOBAL");
            const int exps[] = {-30, -27, -24, -24, 11, 14, 16, 16};
            for (int i = 0; i < 8; ++i)
                fields[std::string(i < 4 ? "alpha" : "beta") +
                       std::to_string(i % 4)] =
                    b.scaled(56 + 8 * i, 8, exps[i]);
            f("a1_s_s", 120, 24, -50);
            fields["a0_s"] = duration(b.scaled(144, 32, -30));
            fields["tot_s"] = seconds(int64_t(b.u(176, 8)) * 4096);
            u("week_raw", 184, 8);
            fields["delta_tls_s"] = int64_t(b.s(192, 8));
            u("wn_lsf_raw", 200, 8);
            u("dn", 208, 8);
            fields["delta_tlsf_s"] = int64_t(b.s(216, 8));
            fields["utc_reference"] = std::string(q ? "UTC_NICT" : "UTC_USNO");
        } else {
            ++counts["unsupported_pages"];
            return;
        }
        Row r = base;
        r.insert(fields.begin(), fields.end());
        emit(kind, r);
        ++counts["decoded_messages"];
        if (!q && (kind == "almanac_entry" || kind == "almanac_epoch" ||
                   kind == "configuration_health"))
            collect_almanac(b, base, fields, key, t);
        if (kind != "special_message" && kind != "qznma_payload" &&
            kind != "nmct" && !std::get<bool>(base.at("alert")))
            remember(kind, key + ":" + std::to_string(id), r, t);
    }
};

BroadcastMessageDecoder::BroadcastMessageDecoder(std::string setup,
                                                 int64_t period)
    : state_(std::make_unique<State>(std::move(setup), period)) {}
BroadcastMessageDecoder::~BroadcastMessageDecoder() = default;
void BroadcastMessageDecoder::discontinuity() {
    std::lock_guard guard(state_->mutex);
    state_->ephemeris.clear();
    state_->almanacs.clear();
    state_->progress.reset();
    state_->next.reset();
    ++state_->counts["discontinuities"];
}
std::map<std::string, uint64_t> BroadcastMessageDecoder::diagnostics() const {
    std::lock_guard guard(state_->mutex);
    auto c = state_->counts;
    c.erase("output_in_call");
    return c;
}
BroadcastMessageDecoder::Output
BroadcastMessageDecoder::feed(ArrowSchema *schema, ArrowArray *array) {
    std::lock_guard guard(state_->mutex);
    auto &s = *state_;
    if (s.failed)
        throw std::runtime_error(
            "Broadcast decoder failed; construct a new instance");
    try {
        ArrowBatchView owner(schema, array);
        Column root{&owner.value, schema};
        auto family = root.child("message_family"), body = root.child("body"),
             time = root.child("nav_epoch_gpst"), checks = root.child("checks"),
             sources = root.child("bitstream_source");
        if (body.v->storage_type != NANOARROW_TYPE_BINARY ||
            checks.v->storage_type != NANOARROW_TYPE_LIST ||
            sources.v->storage_type != NANOARROW_TYPE_LIST)
            throw std::runtime_error("Invalid RawBits body/lists");
        Column check{checks.v->children[0], checks.s->children[0]},
            source{sources.v->children[0], sources.s->children[0]};
        auto result = check.child("result");
        s.counts["output_in_call"] = 0;
        for (int64_t i = 0; i < array->length; ++i) {
            auto row = i + owner.value.offset;
            if (root.child("setup_id").str(row) != s.setup)
                throw std::runtime_error("Mixed broadcast Setup");
            auto t = time.time(row);
            if (t)
                s.advance(*t);
            auto fam = family.str(row);
            if (fam != "GPS_LNAV" && fam != "QZS_LNAV") {
                ++s.counts["unsupported_families"];
                continue;
            }
            auto sys = root.child("satellite_system").str(row);
            auto sat = root.child("satellite_number").integer(row);
            if (sys != (fam == "GPS_LNAV" ? "G" : "J") || sat < 1 ||
                sat > 255 ||
                root.child("body_format").str(row) != "LNAV_300_V1" ||
                root.child("content_kind").str(row) != "navigation_bits" ||
                root.child("bit_length").integer(row) != 300 ||
                root.child("completeness").str(row) != "complete")
                throw std::runtime_error("Invalid LNAV identity/layout");
            bool pass = false, fail = false;
            if (!checks.null(row))
                for (auto j = ArrowArrayViewListChildOffset(
                         checks.v, row + checks.v->offset);
                     j < ArrowArrayViewListChildOffset(
                             checks.v, row + checks.v->offset + 1);
                     ++j) {
                    auto r = result.str(j + check.v->offset);
                    pass |= r == "pass";
                    fail |= r == "fail";
                }
            if (!pass || fail) {
                ++s.counts["rejected_checks"];
                continue;
            }
            if (body.null(row) || sources.null(row))
                throw std::runtime_error("Null LNAV body/source");
            std::vector<std::string> signals;
            for (auto j = ArrowArrayViewListChildOffset(
                     sources.v, row + sources.v->offset);
                 j < ArrowArrayViewListChildOffset(sources.v,
                                                   row + sources.v->offset + 1);
                 ++j)
                signals.push_back(source.str(j));
            if (signals.empty())
                throw std::runtime_error("Empty bitstream source");
            auto bytes = ArrowArrayViewGetBytesUnsafe(body.v, row);
            if (bytes.size_bytes != 38 || (bytes.data.as_uint8[37] & 15))
                throw std::runtime_error("Invalid LNAV size/padding");
            Bits b;
            for (int w = 0; w < 10; ++w)
                for (int k = 0; k < 24; ++k) {
                    int p = w * 30 + k;
                    b.data[(w * 24 + k) / 8] |=
                        ((bytes.data.as_uint8[p / 8] >> (7 - p % 8)) & 1)
                        << (7 - k % 8);
                }
            auto ordered = signals;
            std::sort(ordered.begin(), ordered.end());
            std::string key = sys + std::to_string(sat);
            for (const auto &v : ordered)
                key += "/" + v;
            if (s.ephemeris.size() + s.almanacs.size() > 1024)
                throw std::runtime_error(
                    "Broadcast source assembly capacity exceeded");
            Row base{
                {"setup_id", s.setup},           {"satellite_system", sys},
                {"broadcasting_satellite", sat}, {"message_family", fam},
                {"bitstream_source", signals},   {"nav_epoch_gpst", tv(t)}};
            ++s.counts["accepted_frames"];
            s.decode(b, std::move(base), key, t);
        }
        Output out;
        for (auto &[kind, rows] : s.output)
            out[kind] = batch(rows);
        s.output.clear();
        return out;
    } catch (...) {
        s.failed = true;
        throw;
    }
}
} // namespace neognss_obs
