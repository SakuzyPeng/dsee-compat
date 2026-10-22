# Repository boundaries

- This repository contains the compatibility implementation only. Never add proprietary libraries, parameter/model data, adapted images, disassembly/decompilation, IDA databases, or firmware download code/URLs.
- Original resources are supplied via an explicit external directory. Do not copy, cache, symlink, upload, or commit them here. Temporary corrupt-input fixtures belong only in temporary test directories.
- Preserve the hash-pinned supported profile. Unknown resources must fail before native execution.
- Build and run the default CTest suite without proprietary resources. Run optional native integration tests locally when external resources are available; never upload those resources to hosted CI.
- Keep build artifacts in the existing build directory; avoid duplicate toolchains, large dependency caches, or copied research workspaces.
- Before committing or pushing, run `python3 tools/check_distribution.py`. Review the actual staged diff as well; the checker cannot establish legal clearance.
- Do not make the repository public or create a release unless explicitly requested. MIT applies only to this repository's own code and documentation.
