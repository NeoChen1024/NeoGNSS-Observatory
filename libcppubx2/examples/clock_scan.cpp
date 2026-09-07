// SPDX-License-Identifier: GPL-3.0-only
// Framing/checksum validation stays native; only clock telemetry crosses stdout.
#include <cppubx2/ubx_reader.hpp>
#include <iostream>
#include <memory>

using namespace UBX;
static void close_file(FILE *file) { fclose(file); }
struct Input {
    FILE *file;
    std::array<uint8_t, 1048576> buffer{};
    size_t cursor = 0, size = 0;
    uint64_t offset = 0;
    static ByteReadResult next(void *context) {
        auto &r = *static_cast<Input *>(context);
        if(r.cursor == r.size) {
            r.size = fread(r.buffer.data(), 1, r.buffer.size(), r.file);
            r.cursor = 0;
            if(!r.size) return {ferror(r.file) ? ReadResult::error : ReadResult::end};
        }
        ++r.offset;
        return {ReadResult::ok, r.buffer[r.cursor++]};
    }
};
static std::string hex(std::span<const uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for(auto b : bytes) { out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
}
int main(int argc, char **argv) {
    if(argc != 2 || std::string_view(argv[1]) == "--help") {
        std::cerr << "Usage: cppubx2_clock_scan INPUT.ubx\n";
        return argc == 2 ? 0 : 2;
    }
    try {
        std::unique_ptr<FILE, decltype(&close_file)> file(fopen(argv[1], "rb"), close_file);
        if(!file) throw std::runtime_error("Cannot open input");
        auto input = std::make_unique<Input>();
        input->file = file.get();
        ubx_buf_t body;
        uint64_t frames = 0;
        while(true) {
            size_t noise = 0;
            const auto before = input->offset;
            const auto result = read_ubx_frame(input.get(), Input::next, body, &noise);
            if(result == ReadResult::end && input->offset == before) break;
            if(result != ReadResult::ok || noise)
                throw std::runtime_error("Invalid framing/truncation at offset " + std::to_string(before));
            ubx_frame frame(body);
            if(!frame.valid) throw std::runtime_error("Invalid checksum at offset " + std::to_string(before));
            ++frames;
            if((frame.class_id == 1 && (frame.msg_id == 0x20 || frame.msg_id == 0x22 || frame.msg_id == 0x61)) ||
               (frame.class_id == 0x0a && frame.msg_id == 0x39) || (frame.class_id == 2 && frame.msg_id == 0x15)) {
                auto payload = std::span<const uint8_t>(frame.payload);
                if(frame.class_id == 2) payload = payload.first(std::min(size_t(16), payload.size()));
                std::cout << before << '\t' << +frame.class_id << '\t' << +frame.msg_id << '\t'
                          << frame.payload.size() << '\t' << hex(payload) << '\n';
            }
        }
        std::cout << "summary\t" << input->offset << '\t' << frames << '\n';
        std::cout.flush();
        if(!std::cout) throw std::runtime_error("Output write failed");
    } catch(const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
