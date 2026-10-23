#include "platform.hpp"
#include <atomic>
#include <chrono>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace dsee {
namespace {
std::string suffix() {
    static std::atomic<uint64_t> serial{0};
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
           std::to_string(serial++);
}
#ifdef _WIN32
FILE *open_temporary(const std::filesystem::path &base, std::filesystem::path &name,
                     DWORD attributes) {
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        name = base;
        name += ".tmp." + suffix();
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               attributes, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)
                continue;
            throw std::runtime_error("Cannot create temporary file");
        }
        int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_BINARY | _O_RDWR);
        if (fd < 0) {
            CloseHandle(h);
            remove_file(name);
            throw std::runtime_error("Cannot open temporary file descriptor");
        }
        FILE *f = _fdopen(fd, "w+b");
        if (!f) {
            _close(fd);
            remove_file(name);
            throw std::runtime_error("Cannot open temporary file stream");
        }
        return f;
    }
    throw std::runtime_error("Cannot allocate a unique temporary name");
}
#endif
} // namespace
std::filesystem::path path_from_utf8(const std::string &value) {
    return std::filesystem::u8path(value);
}
std::string path_to_utf8(const std::filesystem::path &value) {
    return value.u8string();
}
#ifdef _WIN32
std::vector<std::string> utf8_arguments(int argc, wchar_t **argv) {
    std::vector<std::string> result;
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr,
                                    nullptr);
        if (!n)
            throw std::runtime_error("Invalid Unicode command line");
        std::string s(size_t(n), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, s.data(), n, nullptr,
                                 nullptr))
            throw std::runtime_error("Cannot encode command line");
        s.pop_back();
        result.push_back(std::move(s));
    }
    return result;
}
#endif
FILE *temporary_file(const std::filesystem::path &destination, std::filesystem::path &name) {
#ifdef _WIN32
    return open_temporary(destination, name, FILE_ATTRIBUTE_NORMAL);
#else
    std::string pattern = destination.string() + ".tmp.XXXXXX";
    std::vector<char> text(pattern.begin(), pattern.end());
    text.push_back(0);
    int fd = mkstemp(text.data());
    if (fd < 0)
        throw std::runtime_error("Cannot create temporary output beside destination");
    name = text.data();
    FILE *f = fdopen(fd, "w+b");
    if (!f) {
        close(fd);
        remove_file(name);
        throw std::runtime_error("Cannot open temporary output");
    }
    return f;
#endif
}
FILE *temporary_spool() {
#ifdef _WIN32
    std::filesystem::path name;
    return open_temporary(std::filesystem::temp_directory_path() / "dsee-pcm", name,
                          FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE);
#else
    FILE *f = std::tmpfile();
    if (!f)
        throw std::runtime_error("Cannot create temporary PCM spool");
    return f;
#endif
}
void sync_file(FILE *f) {
    if (std::fflush(f))
        throw std::runtime_error("Cannot flush output");
#ifdef _WIN32
    if (!FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)))))
#else
    if (fsync(fileno(f)))
#endif
        throw std::runtime_error("Cannot sync output");
}
int seek_file(FILE *f, int64_t offset, int origin) {
#ifdef _WIN32
    return _fseeki64(f, offset, origin);
#else
    static_assert(sizeof(off_t) >= sizeof(int64_t), "64-bit file offsets required");
    return fseeko(f, off_t(offset), origin);
#endif
}
void publish_file(const std::filesystem::path &temporary,
                  const std::filesystem::path &destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish output (destination may already exist)");
#else
    if (link(temporary.c_str(), destination.c_str()))
        throw std::runtime_error("Cannot publish output (destination may already exist)");
    remove_file(temporary);
#endif
}
void remove_file(const std::filesystem::path &p) noexcept {
    std::error_code error;
    std::filesystem::remove(p, error);
}
std::filesystem::path temporary_directory(const char *prefix) {
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        auto path = std::filesystem::temp_directory_path() / (std::string(prefix) + suffix());
        if (std::filesystem::create_directory(path))
            return path;
    }
    throw std::runtime_error("Cannot create temporary directory");
}
} // namespace dsee
