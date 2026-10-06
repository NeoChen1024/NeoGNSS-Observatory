// SPDX-License-Identifier: GPL-3.0-only
// Included inside cnex_detail, after the shared Arrow scalar helpers.
struct TelemetryField {
    const char *name;
    ArrowType type;
};
constexpr TelemetryField telemetry_fields[] = {
    {"setup_id", NANOARROW_TYPE_STRING},
    {"gpst", NANOARROW_TYPE_DECIMAL128},
    {"anchor_gpst", NANOARROW_TYPE_DECIMAL128},
    {"frame_index", NANOARROW_TYPE_UINT64},
    {"receiver_uptime_s", NANOARROW_TYPE_DECIMAL128},
    {"receiver_temperature_c", NANOARROW_TYPE_FLOAT},
    {"cpu_load_percent", NANOARROW_TYPE_FLOAT},
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
        ArrowSchemaSetTypeStruct(&b->schema, std::size(telemetry_fields) + 2));
    size_t i = 0;
    for (auto f : telemetry_fields)
        telemetry_field(b->schema.children[i++], f,
                        std::string_view(f.name) != "setup_id" &&
                            std::string_view(f.name) != "frame_index" &&
                            std::string_view(f.name) != "collection_complete" &&
                            std::string_view(f.name) != "_archive_day");
    auto m = b->schema.children[i++];
    field(m, "measurement_clock", NANOARROW_TYPE_LIST, false);
    telemetry_struct(
        m->children[0], "item",
        {{"gpst", NANOARROW_TYPE_DECIMAL128},
         {"adjustment_reported", NANOARROW_TYPE_BOOL},
         {"cumulative_adjustment_ms", NANOARROW_TYPE_UINT64},
         {"cumulative_adjustment_modulus_ms", NANOARROW_TYPE_UINT64}});
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
    b->init();
    return b;
}

// Typed assembly state. Reports arrive as these structs; JSON appears only in
// the checkpoint, where field names match the ParquetNEX columns.
struct TelemetryClockItem {
    Tick gpst = 0;
    std::optional<bool> adjustment_reported;
    std::optional<uint64_t> cumulative_adjustment_ms,
        cumulative_adjustment_modulus_ms;
};
struct TelemetryPulseItem {
    std::optional<Tick> gpst;
    std::string_view reference_time_scale;
    std::optional<Tick> quantization_error_s;
    std::optional<bool> quantization_error_valid, locked;
    std::optional<std::string_view> raim_status;
    std::optional<uint8_t> utc_standard;
    std::optional<bool> utc_available;
    std::optional<Tick> sync_age_s;
    std::optional<bool> sync_age_saturated;
};
struct TelemetryStatus {
    std::optional<Tick> gpst;
    Tick receiver_uptime_s = 0;
    std::optional<double> receiver_temperature_c, cpu_load_percent;
};
struct TelemetryClock {
    std::optional<Tick> gpst, clock_bias_s;
    std::optional<int64_t> clock_frequency_offset;
    std::optional<Tick> time_accuracy_s;
    std::optional<uint64_t> frequency_accuracy;
    std::string_view clock_reference_time_scale;
};
struct TelemetryRow {
    // present mirrors a non-empty collection window: opened by a navigation
    // block or touched by any report, even one whose scalars are all null.
    bool present = false, located = false;
    std::optional<Tick> anchor_gpst;
    uint64_t frame_index = 0;
    std::optional<Tick> gpst, receiver_uptime_s;
    std::optional<double> receiver_temperature_c, cpu_load_percent;
    std::optional<Tick> clock_bias_s;
    std::optional<int64_t> clock_frequency_offset;
    std::optional<Tick> time_accuracy_s;
    std::optional<uint64_t> frequency_accuracy;
    std::optional<std::string_view> clock_reference_time_scale;
    bool collection_complete = false;
    int64_t archive_day = 0;
    std::vector<TelemetryClockItem> measurement_clock;
    std::vector<TelemetryPulseItem> pulse_timing;
    size_t items() const {
        return measurement_clock.size() + pulse_timing.size();
    }
};
// Enumerated telemetry strings are literals; a checkpoint restores them to the
// same static storage so string_view fields never dangle.
std::string_view telemetry_literal(const std::string &s) {
    static constexpr std::string_view known[] = {
        "GPST",    "UTC",         "RECEIVER",
        "GST",     "BDT",         "FUGRO_ATOMICHRON",
        "UNKNOWN", "UNAVAILABLE", "NOT_ACTIVE",
        "ACTIVE"};
    for (auto k : known)
        if (k == s)
            return k;
    throw std::runtime_error("Unknown telemetry string in checkpoint: " + s);
}
template <class T>
void telemetry_merge_scalar(std::optional<T> &into,
                            const std::optional<T> &from, const char *name) {
    if (!from)
        return;
    if (into && *into != *from)
        throw std::runtime_error(std::string("Conflicting telemetry field: ") +
                                 name);
    into = from;
}
void telemetry_merge(TelemetryRow &row, const TelemetryRow &values) {
    // Nulls never overwrite; the arrival coordinate of the destination wins.
    telemetry_merge_scalar(row.gpst, values.gpst, "gpst");
    telemetry_merge_scalar(row.receiver_uptime_s, values.receiver_uptime_s,
                           "receiver_uptime_s");
    telemetry_merge_scalar(row.receiver_temperature_c,
                           values.receiver_temperature_c,
                           "receiver_temperature_c");
    telemetry_merge_scalar(row.cpu_load_percent, values.cpu_load_percent,
                           "cpu_load_percent");
    telemetry_merge_scalar(row.clock_bias_s, values.clock_bias_s,
                           "clock_bias_s");
    telemetry_merge_scalar(row.clock_frequency_offset,
                           values.clock_frequency_offset,
                           "clock_frequency_offset");
    telemetry_merge_scalar(row.time_accuracy_s, values.time_accuracy_s,
                           "time_accuracy_s");
    telemetry_merge_scalar(row.frequency_accuracy, values.frequency_accuracy,
                           "frequency_accuracy");
    telemetry_merge_scalar(row.clock_reference_time_scale,
                           values.clock_reference_time_scale,
                           "clock_reference_time_scale");
    row.measurement_clock.insert(row.measurement_clock.end(),
                                 values.measurement_clock.begin(),
                                 values.measurement_clock.end());
    row.pulse_timing.insert(row.pulse_timing.end(), values.pulse_timing.begin(),
                            values.pulse_timing.end());
    row.present = row.present || values.present;
}
template <class T> void optional_integer(ArrowArray *a, std::optional<T> v) {
    if (v)
        integer(a, int64_t(*v));
    else
        check(ArrowArrayAppendNull(a, 1));
}
void optional_number(ArrowArray *a, std::optional<double> v) {
    if (v)
        number(a, *v);
    else
        check(ArrowArrayAppendNull(a, 1));
}
void optional_str(ArrowArray *a, std::optional<std::string_view> v) {
    if (v)
        str(a, *v);
    else
        check(ArrowArrayAppendNull(a, 1));
}
void telemetry_append(Batch &b, const TelemetryRow &row,
                      const std::string &id) {
    auto c = b.array.children;
    str(c[0], id);
    optional_decimal(c[1], row.gpst);
    optional_decimal(c[2], row.anchor_gpst);
    integer(c[3], int64_t(row.frame_index));
    optional_decimal(c[4], row.receiver_uptime_s);
    optional_number(c[5], row.receiver_temperature_c);
    optional_number(c[6], row.cpu_load_percent);
    optional_decimal(c[7], row.clock_bias_s);
    optional_integer(c[8], row.clock_frequency_offset);
    optional_decimal(c[9], row.time_accuracy_s);
    optional_integer(c[10], row.frequency_accuracy);
    optional_str(c[11], row.clock_reference_time_scale);
    integer(c[12], row.collection_complete);
    integer(c[13], row.archive_day);
    auto m = c[14]->children[0];
    for (const auto &item : row.measurement_clock) {
        decimal(m->children[0], item.gpst);
        optional_integer(m->children[1], item.adjustment_reported);
        optional_integer(m->children[2], item.cumulative_adjustment_ms);
        optional_integer(m->children[3], item.cumulative_adjustment_modulus_ms);
        check(ArrowArrayFinishElement(m));
    }
    check(ArrowArrayFinishElement(c[14]));
    auto p = c[15]->children[0];
    for (const auto &item : row.pulse_timing) {
        optional_decimal(p->children[0], item.gpst);
        str(p->children[1], item.reference_time_scale);
        optional_decimal(p->children[2], item.quantization_error_s);
        optional_integer(p->children[3], item.quantization_error_valid);
        optional_integer(p->children[4], item.locked);
        optional_str(p->children[5], item.raim_status);
        optional_integer(p->children[6], item.utc_standard);
        optional_integer(p->children[7], item.utc_available);
        optional_decimal(p->children[8], item.sync_age_s);
        optional_integer(p->children[9], item.sync_age_saturated);
        check(ArrowArrayFinishElement(p));
    }
    check(ArrowArrayFinishElement(c[15]));
    check(ArrowArrayFinishElement(&b.array));
}

// Checkpoint serialization. Keys follow the ParquetNEX column names.
Json telemetry_json(std::optional<Tick> t) {
    return t ? time_parts(*t) : Json(nullptr);
}
template <class T> Json telemetry_json(const std::optional<T> &v) {
    return v ? Json(*v) : Json(nullptr);
}
Json telemetry_json(const std::optional<std::string_view> &v) {
    return v ? Json(std::string(*v)) : Json(nullptr);
}
Json telemetry_json(const TelemetryRow &row) {
    Json r = Json::object();
    if (!row.present)
        return r;
    if (row.located) {
        r["anchor_gpst"] = telemetry_json(row.anchor_gpst);
        r["frame_index"] = row.frame_index;
    }
    r["gpst"] = telemetry_json(row.gpst);
    r["receiver_uptime_s"] = telemetry_json(row.receiver_uptime_s);
    r["receiver_temperature_c"] = telemetry_json(row.receiver_temperature_c);
    r["cpu_load_percent"] = telemetry_json(row.cpu_load_percent);
    r["clock_bias_s"] = telemetry_json(row.clock_bias_s);
    r["clock_frequency_offset"] = telemetry_json(row.clock_frequency_offset);
    r["time_accuracy_s"] = telemetry_json(row.time_accuracy_s);
    r["frequency_accuracy"] = telemetry_json(row.frequency_accuracy);
    r["clock_reference_time_scale"] =
        telemetry_json(row.clock_reference_time_scale);
    r["collection_complete"] = row.collection_complete;
    r["_archive_day"] = row.archive_day;
    auto &clock = r["measurement_clock"] = Json::array();
    for (const auto &item : row.measurement_clock)
        clock.push_back(
            {{"gpst", time_parts(item.gpst)},
             {"adjustment_reported", telemetry_json(item.adjustment_reported)},
             {"cumulative_adjustment_ms",
              telemetry_json(item.cumulative_adjustment_ms)},
             {"cumulative_adjustment_modulus_ms",
              telemetry_json(item.cumulative_adjustment_modulus_ms)}});
    auto &pulse = r["pulse_timing"] = Json::array();
    for (const auto &item : row.pulse_timing)
        pulse.push_back(
            {{"gpst", telemetry_json(item.gpst)},
             {"reference_time_scale", std::string(item.reference_time_scale)},
             {"quantization_error_s",
              telemetry_json(item.quantization_error_s)},
             {"quantization_error_valid",
              telemetry_json(item.quantization_error_valid)},
             {"locked", telemetry_json(item.locked)},
             {"raim_status", telemetry_json(item.raim_status)},
             {"utc_standard", telemetry_json(item.utc_standard)},
             {"utc_available", telemetry_json(item.utc_available)},
             {"sync_age_s", telemetry_json(item.sync_age_s)},
             {"sync_age_saturated", telemetry_json(item.sync_age_saturated)}});
    return r;
}
std::optional<Tick> telemetry_tick(const Json &v) {
    if (v.is_null())
        return {};
    auto t = v.get<std::pair<int64_t, int64_t>>();
    if (t.second <= -ps || t.second >= ps)
        throw std::runtime_error("Invalid checkpoint time fraction");
    return Tick(t.first) * ps + t.second;
}
template <class T> std::optional<T> telemetry_value(const Json &v) {
    if (v.is_null())
        return {};
    return v.get<T>();
}
std::optional<std::string_view> telemetry_name(const Json &v) {
    if (v.is_null())
        return {};
    return telemetry_literal(v.get<std::string>());
}
TelemetryRow telemetry_row(const Json &r) {
    TelemetryRow row;
    if (r.empty())
        return row;
    row.present = true;
    if (r.contains("frame_index")) {
        row.located = true;
        row.anchor_gpst = telemetry_tick(r.at("anchor_gpst"));
        row.frame_index = r.at("frame_index").get<uint64_t>();
    }
    row.gpst = telemetry_tick(r.at("gpst"));
    row.receiver_uptime_s = telemetry_tick(r.at("receiver_uptime_s"));
    row.receiver_temperature_c =
        telemetry_value<double>(r.at("receiver_temperature_c"));
    row.cpu_load_percent = telemetry_value<double>(r.at("cpu_load_percent"));
    row.clock_bias_s = telemetry_tick(r.at("clock_bias_s"));
    row.clock_frequency_offset =
        telemetry_value<int64_t>(r.at("clock_frequency_offset"));
    row.time_accuracy_s = telemetry_tick(r.at("time_accuracy_s"));
    row.frequency_accuracy =
        telemetry_value<uint64_t>(r.at("frequency_accuracy"));
    row.clock_reference_time_scale =
        telemetry_name(r.at("clock_reference_time_scale"));
    row.collection_complete = r.at("collection_complete").get<bool>();
    row.archive_day = r.at("_archive_day").get<int64_t>();
    for (const auto &item : r.at("measurement_clock")) {
        TelemetryClockItem c;
        c.gpst = *telemetry_tick(item.at("gpst"));
        c.adjustment_reported =
            telemetry_value<bool>(item.at("adjustment_reported"));
        c.cumulative_adjustment_ms =
            telemetry_value<uint64_t>(item.at("cumulative_adjustment_ms"));
        c.cumulative_adjustment_modulus_ms = telemetry_value<uint64_t>(
            item.at("cumulative_adjustment_modulus_ms"));
        row.measurement_clock.push_back(c);
    }
    for (const auto &item : r.at("pulse_timing")) {
        TelemetryPulseItem p;
        p.gpst = telemetry_tick(item.at("gpst"));
        p.reference_time_scale = telemetry_literal(
            item.at("reference_time_scale").get<std::string>());
        p.quantization_error_s =
            telemetry_tick(item.at("quantization_error_s"));
        p.quantization_error_valid =
            telemetry_value<bool>(item.at("quantization_error_valid"));
        p.locked = telemetry_value<bool>(item.at("locked"));
        p.raim_status = telemetry_name(item.at("raim_status"));
        p.utc_standard = telemetry_value<uint8_t>(item.at("utc_standard"));
        p.utc_available = telemetry_value<bool>(item.at("utc_available"));
        p.sync_age_s = telemetry_tick(item.at("sync_age_s"));
        p.sync_age_saturated =
            telemetry_value<bool>(item.at("sync_age_saturated"));
        row.pulse_timing.push_back(p);
    }
    return row;
}

struct TelemetryAssembler {
    TelemetryRow current, before;
    std::map<int64_t, TelemetryRow> future;
    std::vector<TelemetryRow> ready;
    std::optional<int64_t> key;
    int64_t archive_day = 0;
    uint64_t partial_rows = 0, updates = 0;
    bool ubx = false;
    std::optional<Tick> order_anchor;
    uint64_t order_index = 0;
    void locate(const Position &position) {
        order_anchor = position.anchor;
        order_index = position.index;
    }
    // A row keeps the arrival coordinate of the frame that first created it.
    TelemetryRow &touch(TelemetryRow &row) {
        if (!row.located) {
            row.located = true;
            row.anchor_gpst = order_anchor;
            row.frame_index = order_index;
        }
        row.present = true;
        return row;
    }
    void publish(bool complete) {
        if (!current.present)
            return;
        touch(current);
        current.collection_complete = complete;
        current.archive_day = archive_day;
        if (!complete)
            ++partial_rows;
        ready.push_back(std::move(current));
        current = TelemetryRow{};
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
                    telemetry_merge(current, it->second);
                    it = future.erase(it);
                }
                publish(key.has_value());
                key = next;
                archive_day = day;
                telemetry_merge(current, before);
                before = TelemetryRow{};
                if (auto it = future.find(next); it != future.end()) {
                    telemetry_merge(current, it->second);
                    future.erase(it);
                }
                // The window is located at the navigation block opening it.
                current.present = current.located = true;
                current.gpst.reset();
                current.anchor_gpst = order_anchor;
                current.frame_index = order_index;
            }
        }
        if (key && navigation && (ubx ? f.id() == 0x0120 : trigger)) {
            const auto millis = int64_t(*navigation / 1000000000);
            if ((ubx ? int64_t(UBX::read_le<uint32_t>(f.payload, 0))
                     : millis) == *key) {
                current.gpst = navigation;
                archive_day = int64_t(*navigation / ps / 86400);
            }
        }
    }
    TelemetryRow &target(std::optional<int64_t> t) {
        if (!ubx && t && (!key || *t != *key))
            return future.try_emplace(*t).first->second;
        return current;
    }
    static std::optional<int64_t> stamp(std::optional<Tick> t) {
        if (!t)
            return {};
        return int64_t(*t / 1000000000);
    }
    void status(const TelemetryStatus &report, bool is_ubx) {
        TelemetryRow fields;
        fields.receiver_uptime_s = report.receiver_uptime_s;
        fields.receiver_temperature_c = report.receiver_temperature_c;
        fields.cpu_load_percent = report.cpu_load_percent;
        if (is_ubx && before.receiver_uptime_s &&
            *before.receiver_uptime_s != report.receiver_uptime_s) {
            touch(before);
            before.collection_complete = false;
            before.archive_day = archive_day;
            ready.push_back(std::move(before));
            before = TelemetryRow{};
            ++partial_rows;
        }
        telemetry_merge(touch(is_ubx ? before : target(stamp(report.gpst))),
                        fields);
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
        TelemetryClockItem item{t, e.adjustment_reported, {}, {}};
        if (e.cumulative_adjustment_ms_mod256) {
            item.cumulative_adjustment_ms = *e.cumulative_adjustment_ms_mod256;
            item.cumulative_adjustment_modulus_ms = 256;
        }
        touch(ubx ? before : target(int64_t(t / 1000000000)))
            .measurement_clock.push_back(item);
        ++updates;
    }
    void pulse(const TelemetryPulseItem &item) {
        touch(target(stamp(item.gpst))).pulse_timing.push_back(item);
        ++updates;
    }
    void clock(const TelemetryClock &report) {
        TelemetryRow values;
        values.clock_bias_s = report.clock_bias_s;
        values.clock_frequency_offset = report.clock_frequency_offset;
        values.time_accuracy_s = report.time_accuracy_s;
        values.frequency_accuracy = report.frequency_accuracy;
        values.clock_reference_time_scale = report.clock_reference_time_scale;
        telemetry_merge(touch(target(stamp(report.gpst))), values);
        ++updates;
    }
    bool has_pending() const {
        return current.present || before.present || !future.empty();
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
        if (before.present) {
            current = std::move(before);
            before = TelemetryRow{};
            publish(false);
        }
        for (auto &[t, r] : future) {
            current = std::move(r);
            publish(false);
        }
        future.clear();
        key.reset();
    }
    // Resource bound: pending windows and list items, not serialized size.
    void bound() {
        if (updates < 128)
            return;
        updates = 0;
        size_t load = future.size() + current.items() + before.items();
        for (const auto &[t, r] : future)
            load += r.items();
        if (load > 65536)
            finish();
    }
    void write(Batch &b, const std::string &id) {
        for (const auto &row : ready)
            telemetry_append(b, row, id);
        ready.clear();
    }
    Json save() const {
        Json groups = Json::array();
        for (const auto &[t, r] : future)
            groups.push_back(Json::array({t, telemetry_json(r)}));
        return {{"current", telemetry_json(current)},
                {"before", telemetry_json(before)},
                {"future", groups},
                {"key", key ? Json(*key) : Json(nullptr)},
                {"archive_day", archive_day},
                {"ubx", ubx},
                {"partial_rows", partial_rows}};
    }
    void restore(const Json &v) {
        current = telemetry_row(v.at("current"));
        before = telemetry_row(v.at("before"));
        future.clear();
        for (const auto &item : v.at("future"))
            future[item[0].get<int64_t>()] = telemetry_row(item[1]);
        key = v.at("key").is_null() ? std::optional<int64_t>{}
                                    : v.at("key").get<int64_t>();
        archive_day = v.at("archive_day");
        ubx = v.at("ubx");
        partial_rows = v.at("partial_rows");
    }
};
