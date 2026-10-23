#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace dsee {
std::filesystem::path path_from_utf8(const std::string &);
std::string path_to_utf8(const std::filesystem::path &);
#ifdef _WIN32
std::vector<std::string> utf8_arguments(int argc, wchar_t **argv);
#endif
FILE *temporary_file(const std::filesystem::path &destination, std::filesystem::path &name);
FILE *temporary_spool();
void sync_file(FILE *);
int seek_file(FILE *, int64_t offset, int origin);
void publish_file(const std::filesystem::path &temporary, const std::filesystem::path &destination);
void remove_file(const std::filesystem::path &) noexcept;
std::filesystem::path temporary_directory(const char *prefix);
} // namespace dsee
