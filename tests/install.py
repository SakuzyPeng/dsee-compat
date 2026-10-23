#!/usr/bin/env python3
"""Verify distribution documents and the installed package's C linkage."""
import argparse
import pathlib
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--build', required=True)
p.add_argument('--cmake', required=True)
p.add_argument('--config', default='Release')
p.add_argument('--docdir', required=True)
p.add_argument('--prefix-path', default='')
p.add_argument('--boost', default='')
a = p.parse_args()
root = pathlib.Path(a.build) / 'package-test'
source = root / 'client'
source.mkdir(parents=True, exist_ok=True)
prefix = root / 'install'
(source / 'main.c').write_text('''#include <dsee_compat.h>
#include <string.h>
int main(void) {
    dsee_context *c = 0;
    if (!strlen(dsee_version())) return 1;
    if (dsee_create("missing", 44100, DSEE_PCM, &c) != DSEE_INVALID_ARGUMENT || c) return 2;
    return dsee_destroy(c) != DSEE_OK;
}
''')
(source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(installed_client LANGUAGES C CXX)
find_package(dsee_compat CONFIG REQUIRED)
add_executable(client main.c)
target_link_libraries(client PRIVATE dsee::dsee_compat)
set_target_properties(client PROPERTIES LINKER_LANGUAGE CXX)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/client-$<CONFIG>.path"
     CONTENT "$<TARGET_FILE:client>")
''')
cache = {}
for line in (pathlib.Path(a.build) / 'CMakeCache.txt').read_text().splitlines():
    if '=' in line and ':' in line.split('=', 1)[0]:
        key, value = line.split('=', 1)
        cache[key.split(':', 1)[0]] = value
commands = [
    [a.cmake, '--install', a.build, '--prefix', str(prefix), '--config', a.config],
    [a.cmake, '-S', str(source), '-B', str(root / 'build'), '-G', cache['CMAKE_GENERATOR'],
     '-DCMAKE_BUILD_TYPE=' + a.config,
     '-DCMAKE_PREFIX_PATH=' + str(prefix) + (';' + a.prefix_path if a.prefix_path else '')],
    [a.cmake, '--build', str(root / 'build'), '--config', a.config],
]
if cache.get('CMAKE_GENERATOR_PLATFORM'):
    commands[1] += ['-A', cache['CMAKE_GENERATOR_PLATFORM']]
if a.boost:
    commands[1] += ['-DBoost_INCLUDE_DIR=' + a.boost, '-DBoost_NO_BOOST_CMAKE=ON',
                    '-DCMAKE_POLICY_DEFAULT_CMP0167=OLD']
for command in commands:
    subprocess.run(command, check=True)
repository = pathlib.Path(__file__).resolve().parents[1]
documents = [repository / 'LICENSE', *sorted((repository / 'docs').glob('*.md'))]
manifest = {pathlib.Path(line) for line in
            (pathlib.Path(a.build) / 'install_manifest.txt').read_text(encoding='utf-8').splitlines()}
for document in documents:
    installed = prefix / a.docdir / document.name
    if installed not in manifest or not installed.is_file():
        raise RuntimeError(f'Missing installed distribution document: {installed}')
    if installed.read_bytes() != document.read_bytes():
        raise RuntimeError(f'Installed distribution document differs from source: {installed}')
executable = (root / 'build' / f'client-{a.config}.path').read_text(encoding='utf-8')
subprocess.run([executable], check=True)
print('Installed license/documentation verified; C client linked and ran without resources')
