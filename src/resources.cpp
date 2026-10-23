#include "resources.hpp"
#include "platform.hpp"
#if defined(_WIN32)
#include <windows.h>

#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace dsee {
std::string sha256(const void *data, size_t size) {
    if (size > UINT32_MAX)
        throw std::runtime_error("Hash input too large");
    std::array<unsigned char, 32> hash{};
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize SHA-256");
    auto status =
        BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(static_cast<const unsigned char *>(data)),
                   ULONG(size), hash.data(), ULONG(hash.size()));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status < 0)
        throw std::runtime_error("Cannot compute SHA-256");
#elif defined(__APPLE__)
    CC_SHA256(data, CC_LONG(size), hash.data());
#else
    if (!SHA256(static_cast<const unsigned char *>(data), size, hash.data()))
        throw std::runtime_error("Cannot compute SHA-256");
#endif
    const char *hex = "0123456789abcdef";
    std::string result;
    for (auto v : hash) {
        result += hex[v >> 4];
        result += hex[v & 15];
    }
    return result;
}
void validate_asset(const Bytes &bytes, const AssetSpec &spec) {
    if (bytes.size() != spec.size)
        throw std::runtime_error(std::string("Unsupported resource size: ") + spec.name);
    if (sha256(bytes.data(), bytes.size()) != spec.sha)
        throw std::runtime_error(std::string("Unsupported resource SHA-256: ") + spec.name);
}
Bytes read_asset(const char *root, const AssetSpec &spec) {
    namespace fs = std::filesystem;
    const auto dir = fs::canonical(path_from_utf8(root));
    const auto path = fs::canonical(dir / spec.name);
    const auto rel = path.lexically_relative(dir);
    if (rel.empty() || rel.is_absolute() || *rel.begin() == ".." || !fs::is_regular_file(path) ||
        fs::file_size(path) != spec.size)
        throw std::runtime_error(std::string("Invalid resource path/size: ") + spec.name);
    Bytes bytes(spec.size);
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size())) ||
        in.peek() != EOF)
        throw std::runtime_error(std::string("Cannot read resource: ") + spec.name);
    validate_asset(bytes, spec);
    return bytes;
}
Resources load_resources(const char *root, int profile) {
    if (profile < -1 || profile > 2)
        throw std::runtime_error("Invalid resource profile");
    Resources resources;
    resources.bytes[0] = read_asset(root, library);
    resources.bytes[1] = read_asset(root, dsx);
    for (int p = 0; p < 3; ++p)
        if (profile < 0 || profile == p)
            resources.bytes[size_t(p) + 2] = read_asset(root, parameters[p]);
    return resources;
}
} // namespace dsee
