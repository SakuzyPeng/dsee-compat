# Distribution boundaries and provenance

This repository contains independently written compatibility code, optional offline postprocessing DSP, interface declarations, synthetic test generators, resource identity hashes and documentation. Its MIT license applies only to those materials. It grants no rights to Sony or other third-party programs, model weights, parameters or trademarks.

The implementation was developed using local binary inspection and behavioral testing. It is **not represented as a strict clean-room implementation**. No decompiler output, proprietary DSP implementation, original machine-code body, model data or firmware image is included in the source tree. Necessary symbol names, object-layout offsets and resource identity information describe the supported interface, not a redistribution of the implementation.

The ELF parser and loader are implemented here. The Android packed-relocation format was checked against the [AOSP linker specification/implementation](https://android.googlesource.com/platform/bionic/+/master/linker/linker_reloc_iterators.h). ABI differences are documented by [Apple](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms). No third-party source implementation is vendored.

The experimental GNU/Linux build links the system OpenSSL library; QEMU and the
AArch64 system libraries are separate runtime dependencies. Their own licenses
apply. No emulator, Linux image, sysroot or Sony resource is distributed by this
repository. See [the experimental build notes](QEMU.md).

The optional [Unicorn probe](UNICORN.md) uses the separately supplied
[Unicorn engine](https://github.com/unicorn-engine/unicorn), whose engine license
is GPLv2. That dependency is not included in this repository, and MIT does not
relicense it or a combined distribution. The probe's resource stream is a local
test input with the same pinned hashes, not a firmware downloader.

The integrated Windows runtime and [Dynarmic probe](DYNARMIC.md) use a separately supplied Dynarmic dependency.
Dynarmic's own code is under 0BSD; its bundled dependencies retain their own
licenses. Dependency sources, build products and synthetic PCM remain outside
the source distribution. No original resource is included in either probe.

## What stays outside the repository

- Sony libraries, firmware and parameter/model files, including copies in archives or Git LFS.
- Patched memory images and generated dumps of complete symbol/address tables.
- Disassembly, decompilation, IDA projects and research databases.
- Original firmware download/mirror integrations and instructions to obtain unauthorized copies.
- Local resource paths and reference data not intended for distribution.

Users supply their own lawfully obtained files. The runtime performs local validation and adaptation only. Possession of a device or file does not by itself establish a right to use its software on another device. The applicable software license and local interoperability/copyright rules still matter. Noncommercial use, an MIT license on this wrapper and private hosting do not resolve those questions automatically.

Do not interpret a passing repository checker as legal clearance. Before any public release, review the exact staged/released contents and applicable licenses. Repository visibility remains private unless explicitly changed by its owner.
