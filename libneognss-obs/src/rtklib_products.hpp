// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "rtklib.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" void ngo_combine_precise_clocks(nav_t *nav);
extern "C" void ngo_interpolate_precise_clocks(nav_t *nav);
extern "C" int ngo_read_raw_precise_clocks(const char *path, nav_t *nav);
extern "C" void ngo_combine_precise_orbits(nav_t *nav);
extern "C" int ngo_read_navigation_header(const char *path, nav_t *nav);

namespace neognss_obs {
// Call only under rtklib_mutex. Cached files are immutable during a run.
struct ProductNavDelete {
    void operator()(nav_t *nav) const {
        freenav(nav, 0x7f);
        std::free(nav->erp.data);
        delete nav;
    }
};
using ProductNav = std::unique_ptr<nav_t, ProductNavDelete>;

template <class T>
void append_product_records(T *&dest, int &count, int &capacity,
                            const T *source, size_t length) {
    if (!length)
        return;
    if (length > size_t(INT_MAX - count))
        throw std::overflow_error("Too many product records");
    auto size = size_t(count) + length;
    auto next = static_cast<T *>(std::realloc(dest, size * sizeof(T)));
    if (!next)
        throw std::bad_alloc();
    dest = next;
    std::copy_n(source, length, dest + count);
    count = capacity = int(size);
}

class ProductFileCache {
    std::map<std::string, ProductNav> files_;

  public:
    void retain(const std::vector<std::string> &paths) {
        std::set<std::string> active(paths.begin(), paths.end());
        std::erase_if(files_, [&](const auto &entry) {
            return !active.contains(entry.first);
        });
    }
    template <class Loader>
    const nav_t &get(const std::string &path, Loader load) {
        auto found = files_.find(path);
        if (found == files_.end()) {
            ProductNav data(new nav_t{});
            load(path, *data);
            found = files_.emplace(path, std::move(data)).first;
        }
        return *found->second;
    }
};

class PreciseClockCache {
    ProductFileCache cache_;

  public:
    void load(nav_t &nav, const std::vector<std::string> &paths) {
        cache_.retain(paths);
        for (const auto &path : paths) {
            const auto &data = cache_.get(path, [](const auto &p, nav_t &v) {
                if (ngo_read_raw_precise_clocks(p.c_str(), &v) <= 0)
                    throw std::runtime_error("Cannot read precise clocks: " +
                                             p);
            });
            // Equal boundary epochs update individually reported satellites,
            // including zeroes. Preserve that case through the original reader.
            if (nav.nc && data.nc &&
                std::abs(timediff(nav.pclk[nav.nc - 1].time,
                                  data.pclk[0].time)) <= 1E-9) {
                if (!readrnxc(path.c_str(), &nav))
                    throw std::runtime_error("Cannot read precise clocks: " +
                                             path);
                continue;
            }
            append_product_records(nav.pclk, nav.nc, nav.ncmax, data.pclk,
                                   data.nc);
            ngo_interpolate_precise_clocks(&nav);
            ngo_combine_precise_clocks(&nav);
            if (!nav.pclk || nav.nc <= 0)
                throw std::bad_alloc();
        }
    }
};

class PppProductCache {
    ProductFileCache orbits_, navigation_, erp_;
    std::string antenna_path_;
    pcvs_t antennas_{};

  public:
    PreciseClockCache clocks;
    ~PppProductCache() { free_pcvs(&antennas_); }
    void orbits(nav_t &nav, const std::vector<std::string> &paths) {
        orbits_.retain(paths);
        for (const auto &path : paths) {
            const auto &data = orbits_.get(path, [](const auto &p, nav_t &v) {
                readsp3(p.c_str(), &v, 0);
            });
            append_product_records(nav.peph, nav.ne, nav.nemax, data.peph,
                                   data.ne);
            if (nav.ne)
                ngo_combine_precise_orbits(&nav);
        }
    }
    void navigation(nav_t &nav, const std::vector<std::string> &paths) {
        navigation_.retain(paths);
        for (const auto &path : paths) {
            const auto &data = navigation_.get(path, [](const auto &p,
                                                        nav_t &v) {
                obs_t observations{};
                sta_t station{};
                int ok = readrnx(p.c_str(), 1, "", &observations, &v, &station);
                freeobs(&observations);
                if (ok <= 0)
                    throw std::runtime_error("Cannot read navigation: " + p);
            });
            // Headers selectively overwrite prior system parameters. Replay
            // the small header, not a zero-filled snapshot of absent fields.
            if (!ngo_read_navigation_header(path.c_str(), &nav))
                throw std::runtime_error("Cannot read navigation header: " +
                                         path);
            append_product_records(nav.eph, nav.n, nav.nmax, data.eph, data.n);
            append_product_records(nav.geph, nav.ng, nav.ngmax, data.geph,
                                   data.ng);
            append_product_records(nav.seph, nav.ns, nav.nsmax, data.seph,
                                   data.ns);
        }
        uniqnav(&nav);
    }
    void erp(nav_t &nav, const std::vector<std::string> &paths) {
        erp_.retain(paths);
        for (const auto &path : paths) {
            const auto &data = erp_.get(path, [](const auto &p, nav_t &v) {
                if (!readerp(p.c_str(), &v.erp))
                    throw std::runtime_error("Cannot read ERP: " + p);
            });
            append_product_records(nav.erp.data, nav.erp.n, nav.erp.nmax,
                                   data.erp.data, data.erp.n);
        }
    }
    void antennas(nav_t &nav, const std::string &path, gtime_t time) {
        if (path != antenna_path_) {
            free_pcvs(&antennas_);
            antennas_ = {};
            antenna_path_.clear();
            if (!readpcv(path.c_str(), &antennas_))
                throw std::runtime_error("Cannot read satellite ANTEX: " +
                                         path);
            antenna_path_ = path;
        }
        for (int i = 0; i < MAXSAT; ++i) {
            const auto *pcv = searchpcv(i + 1, "", time, &antennas_);
            nav.pcvs[i] = pcv ? *pcv : pcv_t{};
        }
    }
};
} // namespace neognss_obs
