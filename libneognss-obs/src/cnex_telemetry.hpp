// SPDX-License-Identifier: GPL-3.0-only
// Included inside cnex_detail, after the shared Arrow scalar helpers.
struct TelemetryField {
    const char *name;
    ArrowType type;
};
constexpr TelemetryField telemetry_fields[] = {
    {"setup_id", NANOARROW_TYPE_STRING},
    {"gpst", NANOARROW_TYPE_DECIMAL128},
    {"receiver_uptime_s", NANOARROW_TYPE_DECIMAL128},
    {"receiver_temperature_c", NANOARROW_TYPE_FLOAT},
    {"cpu_load_percent", NANOARROW_TYPE_FLOAT},
    {"cpu_load_max_percent", NANOARROW_TYPE_FLOAT},
    {"memory_usage_percent", NANOARROW_TYPE_FLOAT},
    {"memory_usage_max_percent", NANOARROW_TYPE_FLOAT},
    {"io_usage_percent", NANOARROW_TYPE_FLOAT},
    {"io_usage_max_percent", NANOARROW_TYPE_FLOAT},
    {"fine_time", NANOARROW_TYPE_BOOL},
    {"clock_bias_s", NANOARROW_TYPE_DECIMAL128},
    {"clock_frequency_offset", NANOARROW_TYPE_INT64},
    {"time_accuracy_s", NANOARROW_TYPE_DECIMAL128},
    {"frequency_accuracy", NANOARROW_TYPE_UINT64},
    {"clock_reference_time_scale", NANOARROW_TYPE_STRING},
    {"collection_complete", NANOARROW_TYPE_BOOL},
    {"_archive_day", NANOARROW_TYPE_INT64}};
void telemetry_field(ArrowSchema *s, TelemetryField f, bool nullable = true) {
    if (f.type == NANOARROW_TYPE_DECIMAL128)
        decfield(s, f.name, nullable);
    else
        field(s, f.name, f.type, nullable);
}
void telemetry_struct(ArrowSchema *s, const char *name,
                      std::initializer_list<TelemetryField> fields) {
    check(ArrowSchemaSetTypeStruct(s, fields.size()));
    check(ArrowSchemaSetName(s, name));
    size_t i = 0;
    for (auto f : fields)
        telemetry_field(s->children[i++], f);
}
std::shared_ptr<Batch> telemetry_schema() {
    auto b = std::make_shared<Batch>();
    check(
        ArrowSchemaSetTypeStruct(&b->schema, std::size(telemetry_fields) + 4));
    size_t i = 0;
    for (auto f : telemetry_fields)
        telemetry_field(b->schema.children[i++], f,
                        std::string_view(f.name) != "setup_id" &&
                            std::string_view(f.name) != "collection_complete" &&
                            std::string_view(f.name) != "_archive_day");
    auto m = b->schema.children[i++];
    field(m, "measurement_clock", NANOARROW_TYPE_LIST, false);
    telemetry_struct(m->children[0], "item",
                     {{"gpst", NANOARROW_TYPE_DECIMAL128},
                      {"adjustment_reported", NANOARROW_TYPE_BOOL},
                      {"cumulative_adjustment_ms", NANOARROW_TYPE_UINT64}});
    m->children[0]->flags &= ~ARROW_FLAG_NULLABLE;
    m->children[0]->children[0]->flags &= ~ARROW_FLAG_NULLABLE;
    auto p = b->schema.children[i++];
    field(p, "pulse_timing", NANOARROW_TYPE_LIST, false);
    telemetry_struct(p->children[0], "item",
                     {{"gpst", NANOARROW_TYPE_DECIMAL128},
                      {"reference_time_scale", NANOARROW_TYPE_STRING},
                      {"quantization_error_s", NANOARROW_TYPE_DECIMAL128},
                      {"quantization_error_valid", NANOARROW_TYPE_BOOL},
                      {"locked", NANOARROW_TYPE_BOOL},
                      {"raim_status", NANOARROW_TYPE_STRING},
                      {"utc_standard", NANOARROW_TYPE_UINT8},
                      {"utc_available", NANOARROW_TYPE_BOOL},
                      {"sync_age_s", NANOARROW_TYPE_DECIMAL128},
                      {"sync_age_saturated", NANOARROW_TYPE_BOOL}});
    p->children[0]->flags &= ~ARROW_FLAG_NULLABLE;
    telemetry_struct(b->schema.children[i++], "ubx_status",
                     {{"boot_type", NANOARROW_TYPE_UINT8},
                      {"notice_count", NANOARROW_TYPE_UINT16},
                      {"warning_count", NANOARROW_TYPE_UINT16},
                      {"error_count", NANOARROW_TYPE_UINT16}});
    auto s = b->schema.children[i++];
    check(ArrowSchemaSetTypeStruct(s, 5));
    check(ArrowSchemaSetName(s, "sbf_status"));
    field(s->children[0], "receiver_state_flags", NANOARROW_TYPE_UINT32);
    field(s->children[1], "receiver_error_flags", NANOARROW_TYPE_UINT32);
    field(s->children[2], "external_error_flags", NANOARROW_TYPE_UINT8);
    field(s->children[3], "command_count", NANOARROW_TYPE_UINT8);
    field(s->children[4], "frontends", NANOARROW_TYPE_LIST, false);
    telemetry_struct(s->children[4]->children[0], "item",
                     {{"frontend_code", NANOARROW_TYPE_UINT8},
                      {"antenna_id", NANOARROW_TYPE_UINT8},
                      {"gain_db", NANOARROW_TYPE_INT8},
                      {"pll_locked", NANOARROW_TYPE_BOOL},
                      {"sample_variance", NANOARROW_TYPE_UINT8},
                      {"blanking_percent", NANOARROW_TYPE_UINT8}});
    s->children[4]->children[0]->flags &= ~ARROW_FLAG_NULLABLE;
    for (int index : {0, 1, 3, 5})
        s->children[4]->children[0]->children[index]->flags &=
            ~ARROW_FLAG_NULLABLE;
    b->init();
    return b;
}
void telemetry_append(ArrowArray *a, const ArrowSchema *s, const Json &v) {
    if (v.is_null()) {
        check(ArrowArrayAppendNull(a, 1));
        return;
    }
    std::string_view format(s->format);
    if (format == "+s") {
        for (int64_t i = 0; i < s->n_children; ++i)
            telemetry_append(a->children[i], s->children[i],
                             v.value(s->children[i]->name, Json(nullptr)));
        check(ArrowArrayFinishElement(a));
    } else if (format == "+l") {
        for (const auto &item : v)
            telemetry_append(a->children[0], s->children[0], item);
        check(ArrowArrayFinishElement(a));
    } else if (format.starts_with("d:"))
        decimal(a, Tick(v.at(0).get<int64_t>()) * ps + v.at(1).get<int64_t>());
    else if (format == "u")
        str(a, v.get<std::string>());
    else if (format == "f" || format == "g")
        number(a, v.get<double>());
    else if (v.is_boolean())
        integer(a, v.get<bool>());
    else
        integer(a, v.get<int64_t>());
}
struct TelemetryAssembler {
    Json current = Json::object(), before = Json::object();
    std::map<int64_t, Json> future;
    std::vector<Json> ready;
    std::optional<int64_t> key;
    int64_t archive_day = 0;
    uint64_t partial_rows = 0, updates = 0;
    bool ubx = false;
    static void merge(Json &row, const Json &values) {
        for (const auto &[name, value] : values.items()) {
            if (value.is_null())
                continue;
            if (name == "measurement_clock" || name == "pulse_timing") {
                if (!row.contains(name))
                    row[name] = Json::array();
                for (const auto &item : value)
                    row[name].push_back(item);
            } else {
                if (row.contains(name) && !row[name].is_null() &&
                    row[name] != value)
                    throw std::runtime_error("Conflicting telemetry field: " +
                                             name);
                row[name] = value;
            }
        }
    }
    static std::optional<int64_t> stamp(const Json &report) {
        const auto t = report.value("gpst", Json(nullptr));
        if (t.is_null())
            return {};
        return t[0].get<int64_t>() * 1000 + t[1].get<int64_t>() / 1000000000;
    }
    void publish(bool complete) {
        if (current.empty())
            return;
        current["collection_complete"] = complete;
        current["_archive_day"] = archive_day;
        if (!complete)
            ++partial_rows;
        ready.push_back(std::move(current));
        current = Json::object();
    }
    void frame(const cppgnss::FrameView &f, std::optional<Tick> navigation,
               int64_t day) {
        ubx = f.protocol() == cppgnss::Protocol::ubx;
        bool trigger =
            ubx ? f.id() == 0x0107 : (f.id() == 4006 || f.id() == 4007);
        if (trigger && f.payload.size() >= (ubx ? 4u : 6u)) {
            int64_t next = UBX::read_le<uint32_t>(f.payload, 0);
            if (!ubx)
                next +=
                    int64_t(UBX::read_le<uint16_t>(f.payload, 4)) * 604800000;
            if (!key || *key != next) {
                // Earlier source-stamped reports belong to the closing window;
                // same-time pre-PVT reports belong to the new window.
                for (auto it = future.begin();
                     it != future.end() && it->first < next;) {
                    merge(current, it->second);
                    it = future.erase(it);
                }
                publish(key.has_value());
                key = next;
                archive_day = day;
                merge(current, before);
                before = Json::object();
                if (future.contains(next)) {
                    merge(current, future.at(next));
                    future.erase(next);
                }
                current["gpst"] = nullptr;
            }
        }
        if (key && navigation && (ubx ? f.id() == 0x0120 : trigger)) {
            const auto millis = int64_t(*navigation / 1000000000);
            if ((ubx ? int64_t(UBX::read_le<uint32_t>(f.payload, 0))
                     : millis) == *key) {
                current["gpst"] = time_parts(*navigation);
                archive_day = int64_t(*navigation / ps / 86400);
            }
        }
    }
    Json &target(std::optional<int64_t> t) {
        if (!ubx && t && (!key || *t != *key))
            return future.try_emplace(*t, Json::object()).first->second;
        return current;
    }
    void status(Json report, bool is_ubx) {
        Json fields = Json::object();
        for (const auto &f : telemetry_fields)
            if (std::string_view(f.name) != "gpst" &&
                std::string_view(f.name) != "setup_id" &&
                std::string_view(f.name) != "_archive_day" &&
                report.contains(f.name))
                fields[f.name] = report[f.name];
        for (const char *name : {"ubx_status", "sbf_status"})
            if (report.contains(name))
                fields[name] = report[name];
        if (is_ubx && before.contains("ubx_status") &&
            before.value("receiver_uptime_s", Json(nullptr)) !=
                fields.value("receiver_uptime_s", Json(nullptr))) {
            before["collection_complete"] = false;
            before["_archive_day"] = archive_day;
            ready.push_back(std::move(before));
            before = Json::object();
            ++partial_rows;
        }
        merge(is_ubx ? before : target(stamp(report)), fields);
        ++updates;
    }
    void measurement(const neognss_obs::Measurements &e) {
        if (!e.adjustment_reported && !e.cumulative_adjustment_ms_mod256)
            return;
        Tick t;
        try {
            t = time_of(e);
        } catch (const std::runtime_error &) {
            return;
        }
        Json item = {{"gpst", time_parts(t)},
                     {"adjustment_reported", e.adjustment_reported
                                                 ? Json(*e.adjustment_reported)
                                                 : Json(nullptr)},
                     {"cumulative_adjustment_ms",
                      e.cumulative_adjustment_ms_mod256
                          ? Json(*e.cumulative_adjustment_ms_mod256)
                          : Json(nullptr)}};
        merge(ubx ? before : target(int64_t(t / 1000000000)),
              {{"measurement_clock", Json::array({item})}});
        ++updates;
    }
    void estimate(Json report, bool pulse) {
        Json values = Json::object();
        if (pulse) {
            Json item = {
                {"gpst", report["gpst"]},
                {"reference_time_scale", report["reference_time_scale"]}};
            const char *names[] = {"quantization_error_s",
                                   "quantization_error_valid",
                                   "locked",
                                   "raim_status",
                                   "utc_standard",
                                   "sync_age_s",
                                   "sync_age_saturated"};
            for (int i = 0; i < 7; ++i)
                item[names[i]] = report["value" + std::to_string(i + 7)];
            item["utc_available"] = report["value15"];
            values["pulse_timing"] = Json::array({item});
        } else {
            values = {
                {"clock_bias_s", report["value7"]},
                {"clock_frequency_offset", report["value8"]},
                {"time_accuracy_s", report["value9"]},
                {"frequency_accuracy", report["value10"]},
                {"clock_reference_time_scale", report["reference_time_scale"]}};
        }
        merge(target(stamp(report)), values);
        ++updates;
    }
    bool has_pending() const {
        return !current.empty() || !before.empty() || !future.empty();
    }
    std::optional<int64_t> safe_day() const {
        if (!has_pending())
            return {};
        auto day = archive_day;
        for (const auto &[t, r] : future)
            day = std::min(day, t / 86400000);
        return day;
    }
    void finish() {
        publish(false);
        if (!before.empty()) {
            current = std::move(before);
            before = Json::object();
            publish(false);
        }
        for (auto &[t, r] : future) {
            current = std::move(r);
            publish(false);
        }
        future.clear();
        key.reset();
    }
    void bound() {
        if (updates < 128)
            return;
        updates = 0;
        if (save().dump().size() > 4 * 1024 * 1024)
            finish();
    }
    void write(Batch &b, const std::string &id) {
        for (auto &row : ready) {
            row["setup_id"] = id;
            if (!row.contains("measurement_clock"))
                row["measurement_clock"] = Json::array();
            if (!row.contains("pulse_timing"))
                row["pulse_timing"] = Json::array();
            telemetry_append(&b.array, &b.schema, row);
        }
        ready.clear();
    }
    Json save() const {
        Json groups = Json::array();
        for (const auto &[t, r] : future)
            groups.push_back(Json::array({t, r}));
        return {{"current", current},
                {"before", before},
                {"future", groups},
                {"key", key ? Json(*key) : Json(nullptr)},
                {"archive_day", archive_day},
                {"ubx", ubx},
                {"partial_rows", partial_rows}};
    }
    void restore(const Json &v) {
        current = v.at("current");
        before = v.at("before");
        future.clear();
        for (const auto &item : v.at("future"))
            future[item[0].get<int64_t>()] = item[1];
        key = v.at("key").is_null() ? std::optional<int64_t>{}
                                    : v.at("key").get<int64_t>();
        archive_day = v.at("archive_day");
        ubx = v.at("ubx");
        partial_rows = v.at("partial_rows");
    }
};
