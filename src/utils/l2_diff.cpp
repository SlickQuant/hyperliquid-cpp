#include <hyperliquid/utils/l2_diff.hpp>

#include <openssl/evp.h>
#include <zlib.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> decode_base64(std::string_view input) {
    // EVP_DecodeBlock requires whitespace-free padded input whose length is a multiple of 4
    std::string clean;
    clean.reserve(input.size());
    for (unsigned char ch : input) {
        if (!std::isspace(ch))
            clean += static_cast<char>(ch);
    }
    if (clean.empty())
        return {};

    std::vector<std::uint8_t> output(clean.size() * 3 / 4);
    const int decoded = EVP_DecodeBlock(
        output.data(),
        reinterpret_cast<const unsigned char*>(clean.data()),
        static_cast<int>(clean.size()));
    if (decoded < 0)
        throw std::invalid_argument("Invalid base64 data");

    // EVP_DecodeBlock counts '=' padding bytes as real output bytes; trim them
    std::size_t padding = 0;
    if (clean.size() >= 2 && clean.back() == '=') {
        ++padding;
        if (clean[clean.size() - 2] == '=')
            ++padding;
    }
    output.resize(static_cast<std::size_t>(decoded) - padding);
    return output;
}

} // namespace

namespace hyperliquid {

nlohmann::json decode_l2_diff(std::string_view compressed_diff) {
    auto compressed = decode_base64(compressed_diff);
    if (compressed.empty())
        throw std::invalid_argument("Empty l2 diff payload");

    if (compressed.size() > static_cast<std::size_t>(std::numeric_limits<uInt>::max()))
        throw std::invalid_argument("l2 diff payload too large");

    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
    stream.avail_in = static_cast<uInt>(compressed.size());

    const int init_result = inflateInit2(&stream, -MAX_WBITS);
    if (init_result != Z_OK)
        throw std::runtime_error("Failed to initialize raw deflate decoder");

    std::string inflated;
    inflated.reserve(compressed.size() * 4);
    std::array<char, 65536> buffer;

    try {
        int result = Z_OK;
        while (result != Z_STREAM_END) {
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());

            result = inflate(&stream, Z_NO_FLUSH);
            if (result != Z_OK && result != Z_STREAM_END) {
                throw std::runtime_error("Failed to inflate l2 diff payload");
            }

            inflated.append(buffer.data(), buffer.size() - stream.avail_out);
        }
    } catch (...) {
        inflateEnd(&stream);
        throw;
    }

    inflateEnd(&stream);
    return nlohmann::json::parse(inflated);
}

} // namespace hyperliquid
