// SPDX-License-Identifier: GPL-3.0-only
#include <cppubx2/ubx_archive.hpp>
#include <openssl/evp.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <memory>

template<class T> void write_le(std::ostream &out, T value) {
    value = UBX::little_to_native(value);
    out.write(reinterpret_cast<const char *>(&value), sizeof value);
}
int main(int argc, char **argv) {
    // Internal worker protocol. The public CLI is implemented with Click.
    if(argc != 3) { std::cerr << "Usage: cppubx2_archive_index INPUT INDEX\n"; return 2; }
    int fd = open(argv[1], O_RDONLY);
    struct stat st{};
    if(fd < 0 || fstat(fd, &st) || st.st_size == 0) {
        if(fd >= 0) close(fd);
        std::cerr << "Cannot open nonempty source\n"; return 1;
    }
    void *mapping = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if(mapping == MAP_FAILED) { close(fd); return 1; }
    madvise(mapping, st.st_size, MADV_SEQUENTIAL);
    const auto bytes = std::span(static_cast<const uint8_t *>(mapping), static_cast<size_t>(st.st_size));
    int result = 0;
    try {
        if(access(argv[2], F_OK) == 0) throw std::runtime_error("Index already exists");
        std::ofstream out(argv[2], std::ios::binary);
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.write("UBXIDX04", 8);
        using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
        Digest ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if(!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
           EVP_DigestUpdate(ctx.get(), bytes.data(), bytes.size()) != 1)
            throw std::runtime_error("SHA256 initialization failed");
        unsigned char hash[32]; unsigned hash_len = 0;
        if(EVP_DigestFinal_ex(ctx.get(), hash, &hash_len) != 1 || hash_len != 32)
            throw std::runtime_error("SHA256 failed");
        out.write(reinterpret_cast<const char *>(hash), 32);
        write_le<uint64_t>(out, bytes.size());
        UBX::scan_archive(bytes, [&](const UBX::ArchiveEpoch &e) {
            write_le(out, e.begin); write_le(out, e.end); write_le(out, e.gpst_ms);
            write_le(out, e.tow_ms); write_le(out, e.fingerprint);
            write_le(out, e.flags); write_le(out, e.frames); write_le(out, e.nav_frames);
            write_le(out, e.gps_week);
        });
        out.close();
        struct stat after{};
        if(fstat(fd, &after) || after.st_size != st.st_size ||
           after.st_mtim.tv_sec != st.st_mtim.tv_sec || after.st_mtim.tv_nsec != st.st_mtim.tv_nsec)
            throw std::runtime_error("Source changed while indexing");
    } catch(const std::exception &e) { std::cerr << e.what() << '\n'; result = 1; }
    munmap(mapping, st.st_size); close(fd);
    return result;
}
