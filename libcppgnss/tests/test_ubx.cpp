#include <cppgnss/ubx.hpp>
#include <cppgnss/ubx_rxm_gen.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace UBX;

using ubx_buf_t = std::vector<uint8_t>;

static ubx_buf_t make_frame(uint8_t class_id, uint8_t msg_id,
                            const ubx_buf_t &payload) {
    ubx_buf_t raw = {class_id, msg_id, static_cast<uint8_t>(payload.size()),
                     static_cast<uint8_t>(payload.size() >> 8)};
    raw.insert(raw.end(), payload.begin(), payload.end());
    uint8_t ck_a = 0, ck_b = 0;
    for (uint8_t byte : raw) {
        ck_a += byte;
        ck_b += ck_a;
    }
    raw.push_back(ck_a);
    raw.push_back(ck_b);
    raw.insert(raw.begin(), {0xb5, 0x62});
    return raw;
}

template <class T>
static cppgnss::ParseResult<T> parse_frame(const ubx_buf_t &wire) {
    std::optional<cppgnss::ParseResult<T>> result;
    cppgnss::StreamDecoder decoder(cppgnss::Protocol::ubx);
    decoder.feed(wire, [&](const cppgnss::FrameView &frame) {
        result = cppgnss::parse<T>(frame);
    });
    decoder.finish();
    assert(result);
    return std::move(*result);
}

template <UbxScalar T>
static void put_le(ubx_buf_t &bytes, size_t offset, T value) {
    const auto wire =
        std::bit_cast<std::array<uint8_t, sizeof(T)>>(little_to_native(value));
    std::copy(wire.begin(), wire.end(), bytes.begin() + offset);
}

int main() {
    const ubx_buf_t bytes = {0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa, 0xf9, 0xf8};
    assert(read_le<uint16_t>(bytes, 0) == 0xfeffu);
    assert(read_le<uint32_t>(bytes, 0) == 0xfcfdfeffu);
    assert(read_le<int16_t>(bytes, 0) == -257);
    assert(read_le<int32_t>(bytes, 0) == -50462977);
    assert(read_le<uint64_t>(bytes, 0) == UINT64_C(0xf8f9fafbfcfdfeff));
    const ubx_buf_t float_bytes = {0x00, 0x00, 0x80, 0x3f};
    assert(read_le<float>(float_bytes, 0) == 1.0f);
    bool threw = false;
    try {
        (void)read_le<uint32_t>(float_bytes, 1);
    } catch (const std::out_of_range &) {
        threw = true;
    }
    assert(threw);

    // Zero-length payloads are legal UBX frames.
    const ubx_buf_t raw = {0xb5, 0x62, 0x01, 0x02, 0x00, 0x00, 0x03, 0x0a};
    cppgnss::StreamDecoder decoder(cppgnss::Protocol::ubx);
    decoder.feed(raw, [](const cppgnss::FrameView &frame) {
        assert(frame.payload.empty());
    });
    decoder.finish();
    assert(decoder.frames == 1);

    ubx_nav_pvt pvt;
    pvt.data.valid_bit = 0x03;
    pvt.data.month = 13;
    assert(!ubx_nav_pvt_semantically_valid(pvt));
    pvt.data.month = 7;
    pvt.data.day = 11;
    pvt.data.fixType = 3;
    assert(ubx_nav_pvt_semantically_valid(pvt));
    assert(ubx_nav_pvt_fix_type(pvt) == "3D");
    for (uint8_t type = 0; type <= 6; ++type) {
        pvt.data.fixType = type;
        pvt.data.flags_bit = 1;
        assert(ubx_nav_pvt_fix_ok(pvt) == (type >= 2 && type <= 5));
        pvt.data.flags_bit = 0;
        assert(!ubx_nav_pvt_fix_ok(pvt));
    }
    pvt.data.fixType = 5;
    pvt.data.flags_bit = 1;
    assert(ubx_nav_pvt_fix_type(pvt) == "TIME");
    pvt.data.valid_bit = 0;
    assert(!ubx_nav_pvt_fix_ok(pvt));

    // A real NAV-PVT wire payload exercises generated decoding and the renamed
    // pyubx2 bitfields. Scaled fields must remain raw integers in this API.
    ubx_buf_t pvt_payload(92, 0);
    pvt_payload[4] = 0xea; // year = 2026, little endian
    pvt_payload[5] = 0x07;
    pvt_payload[6] = 9;
    pvt_payload[7] = 5;
    pvt_payload[11] = 3;    // validDate | validTime
    pvt_payload[20] = 3;    // 3D fix
    pvt_payload[21] = 3;    // gnssFixOK | diffSoln
    pvt_payload[22] = 0xe0; // confirmed date/time flags
    pvt_payload[24] = 0xfe; // lon = -2, raw 1e-7 degree units
    pvt_payload[25] = pvt_payload[26] = pvt_payload[27] = 0xff;
    pvt_payload[76] = 0x7b; // pDOP = 123, raw 0.01 units
    auto decoded_result = parse_frame<ubx_nav_pvt>(
        make_frame(UBX_CLASS_NAV, UBX_NAV_PVT, pvt_payload));
    assert(decoded_result);
    const auto &decoded = decoded_result.value();
    assert(decoded.data.year == 2026);
    assert(decoded.data.valid_bit == 3);
    assert(decoded.data.flags_bit == 3);
    assert(decoded.data.flags2_bit == 0xe0);
    assert(decoded.data.lon == -2);
    assert(decoded.data.pDOP == 123);
    assert(ubx_nav_pvt_semantically_valid(decoded));
    assert(ubx_nav_pvt_fix_type(decoded) == "3D/DGNSS");
    pvt_payload.pop_back();
    assert(!parse_frame<ubx_nav_pvt>(
        make_frame(UBX_CLASS_NAV, UBX_NAV_PVT, pvt_payload)));

    assert(ubx_msg_name(0x06, 0x8a) == "CFG-VALSET");
    assert(ubx_msg_name(0x02, 0x36) == "RXM-SPARTN-KEY");
    assert(ubx_msg_name(0x10, 0x02) == "ESF-MEAS");
    assert(ubx_msg_name(0x29, 0x07) == "NAV2-PVT");
    assert(ubx_msg_name(0x13, 0x00) == "MGA-GPS");
    assert(ubx_msg_name(0x13, 0x60) == "MGA-ACK/NAK");
    assert(ubx_msg_name(0x13, 0x00, ubx_buf_t{1}) == "MGA-GPS-EPH");
    assert(ubx_msg_name(0x13, 0x60, ubx_buf_t{0}) == "MGA-NAK-DATA0");
    assert(ubx_msg_name(0x13, 0x60, ubx_buf_t{1}) == "MGA-ACK-DATA0");
    assert(ubx_msg_name(0x13, 0x00, ubx_buf_t{0xff}) == "MGA-GPS");
    assert(ubx_msg_name(0x01, 0xfe) == "NAV-0xfe");
    assert(ubx_msg_name(0xff, 0xfe) == "0xff-0xfe");
    assert(ubx_msg_name(0xf1, 0) == "UBX-00");

    // Observation and navigation-word containers are usable without the logger.
    ubx_buf_t rawx_payload(48, 0);
    put_le<double>(rawx_payload, 0, 123456.25);
    put_le<uint16_t>(rawx_payload, 8, 2400);
    rawx_payload[10] = 18;
    rawx_payload[11] = 1;
    rawx_payload[12] = 3;
    put_le<double>(rawx_payload, 16, 20200000.5);
    put_le<double>(rawx_payload, 24, -1234.5);
    put_le<float>(rawx_payload, 32, -12.5f);
    rawx_payload[36] = 2;
    rawx_payload[37] = 11;
    rawx_payload[38] = 5;
    put_le<uint16_t>(rawx_payload, 40, 1234);
    rawx_payload[46] = 3;
    auto rawx_result = parse_frame<ubx_rxm_rawx>(
        make_frame(UBX_CLASS_RXM, UBX_RXM_RAWX, rawx_payload));
    assert(rawx_result);
    const auto &rawx = rawx_result.value();
    assert(rawx.meas_grp.size() == 1);
    assert(rawx.rcvTow == 123456.25 && rawx.week == 2400 && rawx.leapS == 18);
    assert(rawx.recStat_bit == 3);
    const auto &measurement = rawx.meas_grp[0];
    assert(measurement.prMes == 20200000.5 && measurement.cpMes == -1234.5);
    assert(measurement.doMes == -12.5f && measurement.locktime == 1234);
    assert(measurement.gnssId == 2 && measurement.svId == 11 &&
           measurement.sigId == 5);
    assert(measurement.trkStat_bit == 3);
    rawx_payload[11] = 2;
    assert(!parse_frame<ubx_rxm_rawx>(
        make_frame(UBX_CLASS_RXM, UBX_RXM_RAWX, rawx_payload)));

    ubx_buf_t sfrbx_payload{0, 7, 0, 0, 2, 0, 2, 0};
    sfrbx_payload.resize(16);
    put_le<uint32_t>(sfrbx_payload, 8, 0x8b123456u);
    put_le<uint32_t>(sfrbx_payload, 12, 0xfedcba98u);
    auto sfrbx_result = parse_frame<ubx_rxm_sfrbx>(
        make_frame(UBX_CLASS_RXM, UBX_RXM_SFRBX, sfrbx_payload));
    assert(sfrbx_result);
    const auto &sfrbx = sfrbx_result.value();
    assert(sfrbx.navdata_grp.size() == 2 && sfrbx.version == 2);
    assert(sfrbx.navdata_grp[0].dwrd == 0x8b123456u);
    assert(sfrbx.navdata_grp[1].dwrd == 0xfedcba98u);
    sfrbx_payload.pop_back();
    assert(!parse_frame<ubx_rxm_sfrbx>(
        make_frame(UBX_CLASS_RXM, UBX_RXM_SFRBX, sfrbx_payload)));

    return 0;
}
