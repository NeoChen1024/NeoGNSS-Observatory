// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <neognss_obs/processing.hpp>
#include <numbers>

namespace neognss_obs {
// Receiver phase range error in meters: -PCO dot LOS + PCV. Subtract from
// phase in meters. ARP displacement and code/group delay are separate models.
class ReceiverAntenna {
    struct Pattern {
        std::array<double, 3> pco;
        double first, last, step, dazi;
        std::vector<double> noazi;
        std::vector<std::vector<double>> azimuth;
        explicit Pattern(const Json &p) {
            pco = p.at("pco_neu_m").get<std::array<double, 3>>();
            auto grid = p.at("zenith_deg").get<std::array<double, 3>>();
            first = grid[0];
            last = grid[1];
            step = grid[2];
            dazi = p.at("dazi_deg");
            noazi = p.at("noazi_pcv_m").get<std::vector<double>>();
            azimuth =
                p.at("azimuth_pcv_m").get<std::vector<std::vector<double>>>();
            auto finite = [](const auto &v) {
                return std::all_of(v.begin(), v.end(),
                                   [](double x) { return std::isfinite(x); });
            };
            if (!finite(pco) || !finite(grid) || !finite(noazi) ||
                !std::isfinite(dazi) || first < 0 || last > 90 ||
                last <= first || step <= 0 || dazi < 0 || dazi > 360 ||
                noazi.size() < 2 ||
                std::abs((last - first) / step - double(noazi.size() - 1)) >
                    1e-8)
                throw std::invalid_argument("Invalid receiver antenna grid");
            if (dazi > 0) {
                if (azimuth.size() < 2 ||
                    std::abs(360 / dazi - double(azimuth.size() - 1)) > 1e-8)
                    throw std::invalid_argument(
                        "Incomplete receiver antenna azimuth grid");
                for (size_t i = 0; i < azimuth.size(); ++i)
                    if (azimuth[i].size() != noazi.size() + 1 ||
                        !finite(azimuth[i]) ||
                        std::abs(azimuth[i][0] - i * dazi) > 1e-6)
                        throw std::invalid_argument(
                            "Invalid receiver antenna azimuth row");
                if (!std::equal(azimuth.front().begin() + 1,
                                azimuth.front().end(),
                                azimuth.back().begin() + 1))
                    throw std::invalid_argument(
                        "Discontinuous receiver antenna azimuth seam");
            } else if (!azimuth.empty()) {
                throw std::invalid_argument(
                    "Unexpected receiver antenna azimuth rows");
            }
        }
        double correction(double az, double el, bool oriented) const {
            double zenith = 90 - el;
            if (zenith < first - 1e-8 || zenith > last + 1e-8)
                throw std::runtime_error(
                    "Direction outside receiver antenna calibration grid");
            double z = std::clamp((zenith - first) / step, 0.0,
                                  double(noazi.size() - 1));
            size_t i = std::min(size_t(z), noazi.size() - 2);
            double w = z - i;
            double pcv = (1 - w) * noazi[i] + w * noazi[i + 1];
            if (oriented && dazi > 0) {
                double a = az / dazi;
                size_t j = std::min(size_t(a), azimuth.size() - 2);
                double v = a - j;
                pcv = (1 - v) * ((1 - w) * azimuth[j][i + 1] +
                                 w * azimuth[j][i + 2]) +
                      v * ((1 - w) * azimuth[j + 1][i + 1] +
                           w * azimuth[j + 1][i + 2]);
            }
            constexpr double rad = std::numbers::pi / 180;
            return -pco[0] * std::cos(az * rad) * std::cos(el * rad) -
                   pco[1] * std::sin(az * rad) * std::cos(el * rad) -
                   pco[2] * std::sin(el * rad) + pcv;
        }
    };
    struct Term {
        double weight;
        Pattern pattern;
    };
    struct Record {
        int64_t start, end;
        std::vector<std::vector<Term>> frequencies;
    };
    std::vector<Record> records_;
    double orientation_ = 0;
    bool oriented_ = false;

  public:
    ReceiverAntenna() = default;
    explicit ReceiverAntenna(const Json &model) {
        if (model.is_null())
            return;
        if (model.at("version") != 1)
            throw std::invalid_argument(
                "Unsupported receiver antenna model version");
        oriented_ = !model.at("azimuth_deg").is_null();
        if (oriented_)
            orientation_ = model.at("azimuth_deg");
        if (!std::isfinite(orientation_) || orientation_ < 0 ||
            orientation_ >= 360)
            throw std::invalid_argument("Invalid antenna orientation");
        size_t nfreq = 0;
        for (const auto &r : model.at("records")) {
            Record record{r.at("start_ns"), r.at("end_ns"), {}};
            if (record.start < 0 || record.end < record.start ||
                (!records_.empty() && record.start <= records_.back().end))
                throw std::invalid_argument(
                    "Invalid antenna validity interval");
            for (const auto &f : r.at("frequencies")) {
                std::vector<Term> terms;
                double sum = 0;
                for (const auto &s : f.at("sources")) {
                    double weight = s.at("weight");
                    if (!std::isfinite(weight) || weight < 0 || weight > 1)
                        throw std::invalid_argument(
                            "Invalid antenna interpolation weight");
                    terms.push_back({weight, Pattern(s.at("pattern"))});
                    sum += weight;
                }
                if (terms.empty() || terms.size() > 2 ||
                    std::abs(sum - 1) > 1e-12)
                    throw std::invalid_argument(
                        "Invalid antenna frequency recipe");
                record.frequencies.push_back(std::move(terms));
            }
            if (record.frequencies.empty() ||
                (nfreq && nfreq != record.frequencies.size()))
                throw std::invalid_argument(
                    "Inconsistent antenna frequency count");
            nfreq = record.frequencies.size();
            records_.push_back(std::move(record));
        }
        if (records_.empty())
            throw std::invalid_argument("Empty receiver antenna model");
    }
    bool enabled() const { return !records_.empty(); }
    bool covers(int64_t ns, double elevation) const {
        if (!enabled())
            return true;
        if (!std::isfinite(elevation))
            return false;
        double zenith = 90 - elevation;
        for (const auto &terms : records_[record_index(ns)].frequencies)
            for (const auto &term : terms)
                if (zenith < term.pattern.first - 1e-8 ||
                    zenith > term.pattern.last + 1e-8)
                    return false;
        return true;
    }
    size_t record_index(int64_t ns) const {
        for (size_t i = 0; i < records_.size(); ++i)
            if (records_[i].start <= ns && ns <= records_[i].end)
                return i;
        throw std::runtime_error(
            "No receiver antenna calibration valid at observation epoch");
    }
    double correction(size_t slot, int64_t ns, double az, double el) const {
        if (!enabled())
            return 0;
        const auto &terms = records_[record_index(ns)].frequencies.at(slot);
        if (!std::isfinite(az) || !std::isfinite(el))
            return NAN;
        az = std::fmod(std::fmod(az - orientation_, 360) + 360, 360);
        double value = 0;
        for (const auto &term : terms)
            value += term.weight * term.pattern.correction(az, el, oriented_);
        return value;
    }
};
} // namespace neognss_obs
