// SPDX-License-Identifier: GPL-3.0-only
#include "broadcast_has.hpp"
#include "broadcast_bits.hpp"
#include <algorithm>
#include <array>
#include <set>

namespace neognss_obs::broadcast_detail {
namespace {
uint8_t mul(uint8_t a, uint8_t b) {
    unsigned x = a, y = b, r = 0;
    while (y) {
        if (y & 1)
            r ^= x;
        y >>= 1;
        x <<= 1;
        if (x & 256)
            x ^= 0x11d;
    }
    return uint8_t(r);
}
uint8_t power(uint8_t x, int n) {
    uint8_t r = 1;
    while (n) {
        if (n & 1)
            r = mul(r, x);
        x = mul(x, x);
        n >>= 1;
    }
    return r;
}
using Matrix = std::array<std::array<uint8_t, 32>, 255>;
const Matrix &generator() {
    static const Matrix matrix = [] {
        // HAS ICD 6.2: narrow-sense RS(255,32), roots alpha^1..alpha^223.
        Bytes g{1};
        for (int i = 1; i <= 223; ++i) {
            Bytes n(g.size() + 1);
            for (size_t j = 0; j < g.size(); ++j) {
                n[j] ^= g[j];
                n[j + 1] ^= mul(g[j], power(2, i));
            }
            g = std::move(n);
        }
        Matrix m{};
        for (int c = 0; c < 32; ++c) {
            std::array<uint8_t, 255> p{};
            p[c] = 1;
            m[c][c] = 1;
            for (int i = 0; i < 32; ++i) {
                auto a = p[i];
                for (int j = 1; j <= 223; ++j)
                    p[i + j] ^= mul(a, g[j]);
            }
            for (int i = 32; i < 255; ++i)
                m[i][c] = p[i];
        }
        return m;
    }();
    return matrix;
}
Bytes recover(const std::map<int, Bytes> &pages, int k) {
    std::vector<Bytes> m;
    for (const auto &[pid, data] : pages) {
        Bytes row(k + 53);
        std::copy_n(generator()[pid - 1].begin(), k, row.begin());
        std::copy(data.begin(), data.end(), row.begin() + k);
        m.push_back(std::move(row));
        if (int(m.size()) == k)
            break;
    }
    for (int c = 0; c < k; ++c) {
        int p = c;
        while (p < k && !m[p][c])
            ++p;
        if (p == k)
            throw std::invalid_argument("Singular HAS page set");
        std::swap(m[p], m[c]);
        auto inv = power(m[c][c], 254);
        for (int j = c; j < k + 53; ++j)
            m[c][j] = mul(m[c][j], inv);
        for (int i = 0; i < k; ++i)
            if (i != c) {
                auto a = m[i][c];
                for (int j = c; j < k + 53; ++j)
                    m[i][j] ^= mul(a, m[c][j]);
            }
    }
    Bytes out;
    for (auto &row : m)
        out.insert(out.end(), row.begin() + k, row.end());
    return out;
}
struct Cursor {
    BroadcastBits b;
    size_t pos = 0;
    explicit Cursor(const Bytes &v) : b(v, v.size() * 8) {}
    int64_t u(size_t n) {
        auto x = b.u(pos, n);
        pos += n;
        return int64_t(x);
    }
    int64_t s(size_t n) {
        auto x = b.s(pos, n);
        pos += n;
        return x;
    }
};
struct Mask {
    int system = 0, navigation = 0;
    std::vector<int64_t> satellites, signals, cells;
    bool operator==(const Mask &) const = default;
};
std::string system(int id) { return id == 0 ? "G" : "E"; }
const int validity[] = {5,   10,  15,  20,  30,  60,   90,   120,
                        180, 240, 300, 600, 900, 1800, 3600, 0};
Scalar scaled(int64_t x, int64_t unavailable, double scale) {
    return x == unavailable ? Scalar{} : Scalar(double(x) * scale);
}
} // namespace
struct HasDecoder::State {
    struct Assembly {
        int size = 0, status = 0;
        Tick first = 0;
        std::map<int, Bytes> pages;
    };
    struct Masks {
        std::vector<Mask> values;
        Tick received = 0;
    };
    struct Issues {
        std::map<std::pair<int, int>, int64_t> refs;
        Tick received = 0;
    };
    struct Source {
        std::map<int, Assembly> assemblies;
        std::map<int, Masks> masks;
        std::map<std::pair<int, int>, Issues> issues;
    };
    std::map<std::string, Source> sources;
};
HasDecoder::HasDecoder() : state_(std::make_unique<State>()) {}
HasDecoder::~HasDecoder() = default;
void HasDecoder::clear() { state_->sources.clear(); }

std::vector<DecodedMessage>
HasDecoder::decode(std::span<const uint8_t> data, const std::string &source,
                   std::optional<Tick> time,
                   std::map<std::string, uint64_t> &counts) {
    BroadcastBits b(data, 486);
    std::vector<DecodedMessage> out;
    auto status = int64_t(b.u(14, 2)), mt = int64_t(b.u(18, 2)),
         mid = int64_t(b.u(20, 5));
    auto size = int(b.u(25, 5)) + 1, pid = int(b.u(30, 8));
    if (b.u(14, 24) == 0xaf3bc3) {
        ++counts["has_dummy"];
        return out;
    }
    Row base{
        {"has_status", status}, {"has_message_type", mt}, {"message_id", mid}};
    auto page = base;
    page["message_size_pages"] = int64_t(size);
    page["page_id"] = int64_t(pid);
    page["encoded_page"] = b.bytes(38, 424);
    out.push_back({"gal_has_page", std::move(page)});
    ++counts["decoded_messages"];
    ++counts["has_pages"];
    if (status == 3) {
        clear();
        out.push_back({"gal_has_status", base});
        return out;
    }
    if (status == 2 || mt != 1 || !pid || !time) {
        ++counts["has_unassembled_pages"];
        return out;
    }
    // Systematic pages k+1..32 are zero padding, never transmitted HAS pages.
    if (pid > size && pid <= 32) {
        ++counts["has_invalid_page"];
        return out;
    }
    if (!state_->sources.contains(source) && state_->sources.size() >= 128)
        throw std::runtime_error("HAS source capacity exceeded");
    auto &s = state_->sources[source];
    std::erase_if(s.masks, [&](const auto &p) {
        return *time - p.second.received >= seconds(1800);
    });
    std::erase_if(s.issues, [&](const auto &p) {
        return *time - p.second.received >= seconds(1800);
    });
    auto &a = s.assemblies[int(mid)];
    auto payload = b.bytes(38, 424);
    if (a.size && (a.size != size || a.status != status ||
                   *time - a.first > seconds(150) ||
                   (a.pages.contains(pid) && a.pages.at(pid) != payload))) {
        a = {};
        ++counts["has_assembly_reset"];
    }
    if (!a.size) {
        a.size = size;
        a.status = int(status);
        a.first = *time;
    }
    a.pages[pid] = std::move(payload);
    if (int(a.pages.size()) < size)
        return out;
    Bytes message = recover(a.pages, size);
    base["first_received_gpst"] = a.first;
    a = {};
    ++counts["has_messages"];
    // Parse transactionally: malformed messages must not install masks/issues.
    auto masks = s.masks;
    auto issues = s.issues;
    size_t begin = out.size();
    try {
        Cursor c(message);
        auto toh = c.u(12), flags = c.u(6);
        c.u(4);
        auto maskid = c.u(5), iod = c.u(5);
        if (toh >= 3600)
            throw std::invalid_argument("HAS TOH out of range");
        Tick epoch = (*time / seconds(3600)) * seconds(3600) + seconds(toh);
        if (epoch > *time)
            epoch -= seconds(3600);
        base["toh_s"] = seconds(toh);
        base["reference_gpst"] = epoch;
        base["mask_id"] = maskid;
        base["iod_set_id"] = iod;
        auto raw = base;
        raw["content_flags"] = flags;
        raw["payload"] = message;
        out.push_back({"gal_has_message", std::move(raw)});
        auto add = [&](const char *kind, Records entries, int vi) {
            Row r = base;
            r["entries"] = std::move(entries);
            r["validity_index"] = int64_t(vi);
            r["valid_until_gpst"] =
                vi < 15 ? Value(epoch + seconds(validity[vi])) : Value{};
            out.push_back({kind, std::move(r), vi < 15,
                           std::to_string(maskid) + "/" + std::to_string(iod)});
        };
        if (flags & 32) {
            int n = int(c.u(4));
            if (!n)
                throw std::invalid_argument("Empty HAS mask");
            std::vector<Mask> value;
            std::set<int> seen;
            for (int i = 0; i < n; ++i) {
                Mask m;
                m.system = int(c.u(4));
                if ((m.system != 0 && m.system != 2) ||
                    !seen.insert(m.system).second)
                    throw std::invalid_argument("Unsupported HAS mask system");
                for (int j = 1; j <= 40; ++j)
                    if (c.u(1))
                        m.satellites.push_back(j);
                for (int j = 0; j < 16; ++j)
                    if (c.u(1))
                        m.signals.push_back(j);
                bool cell = c.u(1);
                for (size_t j = 0; j < m.satellites.size() * m.signals.size();
                     ++j)
                    m.cells.push_back(cell ? c.u(1) : 1);
                m.navigation = int(c.u(3));
                value.push_back(std::move(m));
            }
            c.u(6);
            if (masks.contains(int(maskid)) &&
                masks.at(int(maskid)).values != value)
                std::erase_if(issues, [&](const auto &p) {
                    return p.first.first == maskid;
                });
            masks[int(maskid)] = {value, *time};
            for (const auto &m : value) {
                auto r = base;
                r["subject_system"] = system(m.system);
                r["navigation_index"] = int64_t(m.navigation);
                r["satellite_ids"] = m.satellites;
                r["signal_indices"] = m.signals;
                r["cell_mask"] = m.cells;
                out.push_back({"gal_has_mask", std::move(r)});
            }
        }
        if (!masks.contains(int(maskid))) {
            ++counts["has_missing_mask"];
            return out;
        }
        const auto &mask = masks.at(int(maskid)).values;
        auto idkey = std::pair{int(maskid), int(iod)};
        State::Issues newissues;
        newissues.received = *time;
        Fields identity{{"subject_system", std::string{}},
                        {"subject_sv_id", int64_t{}},
                        {"navigation_index", int64_t{}},
                        {"iod_reference", int64_t{}}};
        auto ident = [&](const Mask &m, int64_t sat) {
            Fields r{{"subject_system", system(m.system)},
                     {"subject_sv_id", sat},
                     {"navigation_index", int64_t(m.navigation)},
                     {"iod_reference", Scalar{}}};
            auto found = issues.find(idkey);
            if (found != issues.end()) {
                auto it = found->second.refs.find({m.system, int(sat)});
                if (it != found->second.refs.end())
                    r["iod_reference"] = it->second;
            }
            return r;
        };
        if (flags & 16) {
            int vi = int(c.u(4));
            Records entries{identity, {}};
            for (auto n : {"radial_m", "along_track_m", "cross_track_m"})
                entries.prototype[n] = 0.0;
            for (const auto &m : mask)
                for (auto sat : m.satellites) {
                    auto r = ident(m, sat);
                    auto ref = c.u(m.system == 0 ? 8 : 10);
                    r["iod_reference"] = ref;
                    newissues.refs[{m.system, int(sat)}] = ref;
                    r["radial_m"] = scaled(c.s(13), -4096, .0025);
                    r["along_track_m"] = scaled(c.s(12), -2048, .008);
                    r["cross_track_m"] = scaled(c.s(12), -2048, .008);
                    entries.rows.push_back(std::move(r));
                }
            issues[idkey] = newissues;
            add("gal_has_orbit", std::move(entries), vi);
        }
        auto clock_proto = identity;
        clock_proto["clock_m"] = 0.0;
        clock_proto["do_not_use"] = false;
        clock_proto["multiplier"] = int64_t{};
        auto clockrow = [&](const Mask &m, int64_t sat, int mult) {
            auto r = ident(m, sat);
            auto x = c.s(13);
            r["clock_m"] = (x == -4096 || x == 4095)
                               ? Scalar{}
                               : Scalar(double(x) * .0025 * mult);
            r["do_not_use"] = x == 4095;
            r["multiplier"] = int64_t(mult);
            return r;
        };
        if (flags & 8) {
            int vi = int(c.u(4));
            std::vector<int> mult;
            for (size_t i = 0; i < mask.size(); ++i)
                mult.push_back(int(c.u(2)) + 1);
            Records entries{clock_proto, {}};
            for (size_t i = 0; i < mask.size(); ++i)
                for (auto sat : mask[i].satellites)
                    entries.rows.push_back(clockrow(mask[i], sat, mult[i]));
            add("gal_has_clock_full", std::move(entries), vi);
        }
        if (flags & 4) {
            int vi = int(c.u(4)), n = int(c.u(4));
            Records entries{clock_proto, {}};
            std::set<int> seen;
            for (int i = 0; i < n; ++i) {
                int sys = int(c.u(4)), mult = int(c.u(2)) + 1;
                auto it = std::find_if(mask.begin(), mask.end(), [&](auto &m) {
                    return m.system == sys;
                });
                if (it == mask.end() || !seen.insert(sys).second)
                    throw std::invalid_argument("Invalid HAS subset");
                std::vector<int64_t> selected;
                for (auto sat : it->satellites)
                    if (c.u(1))
                        selected.push_back(sat);
                for (auto sat : selected)
                    entries.rows.push_back(clockrow(*it, sat, mult));
            }
            add("gal_has_clock_subset", std::move(entries), vi);
        }
        for (int phase = 0; phase < 2; ++phase)
            if (flags & (phase ? 1 : 2)) {
                int vi = int(c.u(4));
                Fields proto = identity;
                proto["signal_index"] = int64_t{};
                proto[phase ? "bias_cycles" : "bias_m"] = 0.0;
                if (phase)
                    proto["discontinuity_indicator"] = int64_t{};
                Records entries{proto, {}};
                for (const auto &m : mask)
                    for (size_t i = 0; i < m.satellites.size(); ++i)
                        for (size_t j = 0; j < m.signals.size(); ++j)
                            if (m.cells[i * m.signals.size() + j]) {
                                auto r = ident(m, m.satellites[i]);
                                r["signal_index"] = m.signals[j];
                                r[phase ? "bias_cycles" : "bias_m"] =
                                    scaled(c.s(11), -1024, phase ? .01 : .02);
                                if (phase)
                                    r["discontinuity_indicator"] = c.u(2);
                                entries.rows.push_back(std::move(r));
                            }
                add(phase ? "gal_has_phase_bias" : "gal_has_code_bias",
                    std::move(entries), vi);
            }
        s.masks = std::move(masks);
        s.issues = std::move(issues);
    } catch (const std::out_of_range &) {
        out.resize(begin);
        ++counts["has_invalid_message"];
    } catch (const std::invalid_argument &) {
        out.resize(begin);
        ++counts["has_invalid_message"];
    }
    return out;
}
} // namespace neognss_obs::broadcast_detail
