#pragma once
#include <cstddef>

namespace dsee {
struct AssetSpec {
    const char *name;
    size_t size;
    const char *sha;
};
inline constexpr AssetSpec library = {
    "vendor/lib64/libsonydseehxwrapper.so", 517392,
    "511f7efcdf99cb00ca9553c23364e89c4eab91bae631b68d1df4dc116fead806"};
inline constexpr AssetSpec dsx = {
    "vendor/etc/dsx_param_file.bin", 60000,
    "dfc87d4b071c8b6a295690daefc2df889e827c393ae0411d54bcd14d5d3956f4"};
inline constexpr AssetSpec parameters[] = {
    {"vendor/etc/DrangeparaPCM48.bin", 426576,
     "a915e3899841af760e40a8f4325c3522356f178a68e2a462dde39680947cfe45"},
    {"vendor/etc/DrangeparaAAC128.bin", 426576,
     "6094fc762f13976c6c1816e4c73dd3a4ef7e6bdec1011971f4a331639f25e48d"},
    {"vendor/etc/DrangeparaMP3_128.bin", 426576,
     "ebd94f7abcd86ecd5d97810b114890c177ce02b8e2bb2c3c92415665d7323aaf"}};
} // namespace dsee
