# Distribution boundaries and provenance

This repository contains independently written compatibility code, interface declarations, synthetic test generators, resource identity hashes and documentation. Its MIT license applies only to those materials. It grants no rights to Sony or other third-party programs, model weights, parameters or trademarks.

The implementation was developed using local binary inspection and behavioral testing. It is **not represented as a strict clean-room implementation**. No decompiler output, DSP implementation, original machine-code body, model data or firmware image is included in the source tree. Necessary symbol names, object-layout offsets and resource identity information describe the supported interface, not a redistribution of the implementation.

The ELF parser and loader are implemented here. The Android packed-relocation format was checked against the [AOSP linker specification/implementation](https://android.googlesource.com/platform/bionic/+/master/linker/linker_reloc_iterators.h). ABI differences are documented by [Apple](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms). No third-party source implementation is vendored.

## What stays outside the repository

- Sony libraries, firmware and parameter/model files, including copies in archives or Git LFS.
- Patched memory images and generated dumps of complete symbol/address tables.
- Disassembly, decompilation, IDA projects and research databases.
- Original firmware download/mirror integrations and instructions to obtain unauthorized copies.
- Local resource paths and reference data not intended for distribution.

Users supply their own lawfully obtained files. The runtime performs local validation and adaptation only. Possession of a device or file does not by itself establish a right to use its software on another device. The applicable software license and local interoperability/copyright rules still matter. Noncommercial use, an MIT license on this wrapper and private hosting do not resolve those questions automatically.

Do not interpret a passing repository checker as legal clearance. Before any public release, review the exact staged/released contents and applicable licenses. Repository visibility remains private unless explicitly changed by its owner.
