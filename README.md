# dsee-compat

**简体中文** · [English](README.en.md)

一个让**一款受支持的 Sony Android 音频库在 Apple Silicon macOS 和 Windows x86-64 上运行**的独立兼容运行时，附带 WAV 命令行工具。
Apple Silicon 原生执行 ARM64 指令，Windows 使用 Dynarmic JIT；两者共用 C API、流式状态机和 WAV 工具。公开接口与后处理使用 C++17，内部 Dynarmic 目标使用 C++20。

> **本仓库不分发任何 Sony 代码、二进制、模型、参数文件、固件下载或改编过的镜像。** 构建时不需要这些资源；
> 运行时请在本地提供你自己的资源。这不是 Sony 产品，也不是官方支持的移植。MIT 只覆盖本仓库自己的代码，
> 不授予任何 Sony 软件的权利。详见[分发边界](docs/DISTRIBUTION.md)。

本文只讲上手和构建。**C API 的完整约定见 [API 文档](docs/API.md)，测试结果和性能数据见[验证记录](docs/VALIDATION.md)。**

## 目录

- [能用它做什么](#能用它做什么)
- [支持范围](#支持范围)
- [从源码构建](#从源码构建)
- [准备外部资源目录](#准备外部资源目录)
- [上手三步](#上手三步)
- [处理流程](#处理流程)
- [可调后处理](#可调后处理)
- [C API](#c-api)
- [常见问题](#常见问题)
- [已知限制](#已知限制)
- [测试](#测试)
- [文档](#文档)
- [许可证](#许可证)

## 能用它做什么

- **在 Mac 或 Windows 上处理 WAV**：把 48 kHz 立体声 PCM16 输入送进原始算法，得到 48 / 96 / 192 kHz 的立体声 PCM32 输出。
- **检查资源是否受支持**：`inspect` 只校验库和三套参数的大小与 SHA-256，不执行任何固件代码。
- **嵌入自己的程序**：静态库（macOS `.a`、Windows `.lib`）加一个纯 C 头文件，按流式接口逐块处理。
- **不碰原始文件**：资源只读入内存校验并使用，不复制、不缓存、不修改，也不写任何补丁后的二进制。
- **安全地写输出**：不覆盖已有文件，先写同目录临时文件，完成后再原子发布；失败时自动清理。

## 支持范围

| 项目 | 支持 |
| --- | --- |
| 默认主机 | Apple Silicon macOS；Windows x86-64（Dynarmic） |
| 固件 | Xperia 5 V `67.1.A.2.229`，按资源精确哈希识别 |
| DSEE 输入 | RIFF/WAVE，格式标签 1，48 kHz，立体声，有符号 PCM16 |
| 输出 | 立体声有符号 PCM32，48、96（默认）或 192 kHz |
| 源元数据配置 | `pcm`（默认）、`aac128`、`mp3128` |

源元数据配置只用来选择算法参数；CLI **不解码** AAC/MP3，也不会推断 WAV 文件原来是什么编码。

| 主机 | 默认后端 | 状态 |
| --- | --- | --- |
| Apple Silicon macOS | NATIVE | 原生 C API / CLI |
| Windows x86-64 | DYNARMIC | 原生 Windows 进程中的 C API / CLI，无需 WSL |
| AArch64 Linux | NATIVE | 需 `DSEE_EXPERIMENTAL_LINUX=ON`，仍为实验性 |

`DSEE_BACKEND=AUTO` 按平台选择，也可显式选择 `NATIVE` 或 `DYNARMIC`；不支持的平台组合在配置时拒绝。
首版集成范围不包含 Intel Mac 或 Linux x86-64。详见 [Dynarmic 构建与验证](docs/DYNARMIC.md)和 [QEMU 历史测试](docs/QEMU.md)。
[Unicorn](docs/UNICORN.md) 保留为独立对照探针。

## 从源码构建

### 需要准备

- macOS：**Apple Silicon** 与 **Xcode Command Line Tools**。
- Windows：**x64 MSVC / Windows SDK**、C++20 支持，以及已安装的固定版本 Dynarmic 和依赖，见 [Windows 构建说明](docs/DYNARMIC.md#integrated-windows-build)。
- **CMake 3.20+**。
- **Python 3**——仅开发测试和仓库检查需要，音频处理运行时不需要。

构建过程不下载任何依赖包，也不需要 Sony 资源。

### 构建与测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

主要产物是 `build/dsee-compat`（CLI）、`build/libdsee_compat.a`（兼容运行时）和
`build/libdsee_postprocess.a`（原生后处理静态库）。
加 `-DBUILD_TESTING=OFF` 可以只构建 CLI 和静态库，不需要 Python。

### 安装

```sh
cmake --install build --prefix <目标目录>
```

会安装 CLI、公开静态库及内部支持库、两个公开头文件，以及传递静态链接依赖的 CMake 包。
`LICENSE` 和公开文档（包括分发边界说明）默认安装到 `share/doc/dsee_compat`；可通过 `CMAKE_INSTALL_DOCDIR` 调整。
Windows 构建还需要在消费项目中提供 Dynarmic 安装前缀；CMake 不下载依赖。

## 准备外部资源目录

准备一个下面这种布局的目录，**不要放进本仓库**：

```text
<resource-root>/
  vendor/lib64/libsonydseehxwrapper.so
  vendor/etc/dsx_param_file.bin
  vendor/etc/DrangeparaPCM48.bin
  vendor/etc/DrangeparaAAC128.bin
  vendor/etc/DrangeparaMP3_128.bin
```

- `inspect` 检查库和全部三套配置；`process` 只需要库、DSX 参数和所选配置对应的 Drange 文件。
- 每个用到的文件都必须与受支持的大小和 SHA-256 完全一致；未知资源会在执行任何原生代码之前被拒绝。
- 指向资源根目录以外的符号链接会被拒绝。
- 校验过的字节直接在内存中使用，处理期间不会重新打开文件；原始文件不会被改动。

## 上手三步

1. 按上一节准备好资源目录。
2. 检查资源是否受支持：

   ```sh
   build/dsee-compat inspect --assets /path/to/resource-root
   ```

   成功时输出一行 JSON，列出固件版本和可用配置。

3. 处理一个 WAV：

   ```sh
   build/dsee-compat process --assets /path/to/resource-root \
     --input input.wav --output output.wav \
     --output-rate 96000 --source-profile pcm
   ```

   完成后输出一行 JSON 摘要：输入帧数、填充帧数、输出帧数、采样率、配置，以及包含输出 I/O 在内的耗时。

`build/dsee-compat --help` 查看用法，`--version` 查看版本。

## 处理流程

- 输入按小缓冲区流式读取。
- 最后不满一块的输入用零补齐到 1024 输入帧。
- 之后持续送入零块，直到连续四块输出全为零为止；排空最多额外 48 个零块，**排空不了就报错**，不会悄悄截断结果。
- 保留算法延迟和零输入产生的尾部，因此**输出可能比输入长**。不做自动对齐、归一化，也不会为不支持的输入重采样。
- 空输入得到一个空 WAV。

## 可调后处理

原生 C++ 后处理支持**自适应频谱包络约束、分频混合及两者叠加**，无需额外运行环境。
可以通过 `process --postprocess mix` 在 DSEE 处理后直接应用，默认参考音频就是输入 WAV；
也可以用独立入口对已有 WAV 反复调参，该入口不需要 Sony 资源：

```sh
build/dsee-compat postprocess \
  --reference input.wav --input dsee.wav --output smooth.wav \
  --mode combined --mix 0.5 --curve cosine \
  --crossover-low 18000 --crossover-high 22000 \
  --envelope-strength 1 --max-cut-db 12
```

- `--mode adaptive|mix|combined|none`：单独包络、单独混合、叠加或原样输出；集成入口用 `--postprocess` 选择。
- `--mix auto`：实验性的整曲自动比例，优先优化交界连续性；`--auto-mix-objective envelope` 可使用原包络策略，JSON 会报告选择依据。
- `--mix 0.5`：高频最多混入 50% 处理信号；`--mix 1`：原信号降至 0%，处理信号升至 100%。
- `--curve linear|cosine`、交接频率、包络强度、最大衰减和时间平滑均可调。
- 自动估计延迟；短音频、静音和周期纯音可用 `--reference-delay` 指定输出采样率下的样本数。
- 后处理接受双声道 PCM16/PCM32 WAV，输出 PCM32 WAV；不处理元数据和封面。

`process` 默认关闭后处理，C API 行为不变。若使用未经降音量的原曲作单独参考，而 DSEE 输入
降低过 3 dB，加 `--reference-gain-db -3`；使用实际输入 WAV 作参考时保持 0。
全部参数、格式限制、C++ 接口和 50%/100% 的含义见[后处理文档](docs/POSTPROCESS.md)。

## C API

包含 `dsee_compat.h`；头文件是合法的 C。建议通过安装后的 CMake target 链接，以传递内部支持库、Dynarmic 和系统依赖：

```cmake
project(my_app LANGUAGES C CXX)
find_package(dsee_compat CONFIG REQUIRED)
add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE dsee::dsee_compat)
set_target_properties(my_app PROPERTIES LINKER_LANGUAGE CXX)
```

将安装目录（Windows 还包括 Dynarmic 前缀）加入 `CMAKE_PREFIX_PATH`。Windows C API 的资源目录字符串使用 UTF-8；CLI 支持中文和空格路径。

```text
dsee_inspect → dsee_create → 多次 dsee_process → 多次 dsee_finish → dsee_destroy
```

- 每个进程同时只能有一个活动实例；调用必须串行，重叠调用和第二个实例返回 `DSEE_BUSY`。
- 计数和容量都以**立体声帧**为单位，不是单个采样。
- 常规校验错误和原生适配层的失败都以状态码返回，错误文本由 `dsee_last_error()` 给出。
- 本库在进程内执行一个按哈希锁定的外部二进制，**它不是沙箱**，无法保证从任意原生崩溃或 ABI 变化中恢复。

完整的生命周期、缓冲区要求和错误语义见 [API 文档](docs/API.md)。

## 常见问题

**为什么提示资源不受支持？**
每个文件都按大小和 SHA-256 精确比对，只认 `67.1.A.2.229` 这一套资源。其他固件版本、修改过的文件，
哪怕只改了一个字节，都会在执行任何代码之前被拒绝。先用 `inspect` 确认资源目录布局和文件是否正确。

**我的 WAV 为什么打不开？**
`process` 输入只接受格式标签 1、48 kHz、立体声、16 位整数 PCM。请先用自己的工具转换成这一格式。
独立 `postprocess` 支持 PCM16/PCM32 和上述后处理采样率；两者都不支持 24 位、浮点、RF64 和 WAVE extensible。

**为什么输出比输入长？**
算法有延迟，处理结束时还会送入零块把尾部排空，这些输出都保留下来，不做裁剪或对齐。
JSON 摘要里的 `padding_frames` 是补进去的零输入帧数。

**`aac128` / `mp3128` 是解码 AAC/MP3 吗？**
不是。它们只是告诉算法源素材原本是什么编码，从而选用对应的 Drange 参数；输入仍然必须是 PCM16 WAV。

**为什么不覆盖输出文件？**
这是有意的。目标文件已存在、输入输出是同一路径，或结果可能超过 RIFF 大小上限时，CLI 都会拒绝。
它先写同目录临时文件，完成后再原子发布，不会替换已有文件；转换失败会删除临时文件。

**能同时开多个实例或多线程处理吗？**
DSEE 运行时每个进程只允许一个活动实例，并发调用返回 `DSEE_BUSY`，可以等另一个调用完成后重试。
独立后处理 C++ API 可用互不共享的音源和输出并行调用。

## 已知限制

- 原生 macOS 路径在 Apple M4 Pro / macOS 27.0.1 上验证；其他 Apple Silicon 机型、其他固件版本尚未验证。
- ARM64 Linux / x86-64 QEMU 路径为实验性，部分输出与 macOS 存在微小数值差异，见 [QEMU 验证说明](docs/QEMU.md)。
- **没有拿 Xperia 真机的输出做过比对**：目前的一致性只针对本地研究用的原始 wrapper 参考结果。
- 不支持 RF64、WAVE extensible、24 位和浮点；DSEE 输入仍限定为 48 kHz PCM16。
- 没有 GUI、没有 Audio Unit；DSEE 运行时不支持多实例并发。
- 不是沙箱：任意原生段错误和主机内存耗尽不在错误处理保证之内。

## 测试

### 默认测试（不需要 Sony 资源）

`ctest` 使用自己生成的 ELF/WAV 夹具，覆盖 ELF 范围与溢出拒绝、RELA/APS2/RELR 解码、WAV 解析、
原子输出、CLI 错误、C 头文件链接和 API 错误。
原生后处理测试使用合成音频，覆盖重建、混合响应、延迟、重采样、包络和边界行为，不需要可选 DSP 依赖。

### 可选的本地集成测试

在本地有外部资源时运行：

```sh
build/api_test /path/to/resource-root
build/api_test /path/to/resource-root benchmark
python3 tests/integration.py --exe build/dsee-compat \
  --assets /path/to/resource-root \
  --reference-results /path/to/external/validation.json \
  --report build/integration.json
```

`--reference-results` 是外部保存的原始 wrapper 合成夹具记录，本仓库不分发。脚本先用已有夹具的 PCM 哈希比对
输出前缀，再检查新 CLI 产生的尾部。测试音频和损坏资源夹具都临时生成、用完删除。
**不要把这些资源上传到托管 CI。**

当前本地结果：36 项原始 wrapper 参考结果一致、9 项配置/采样率组合的尾部比对、分片输入等价、并发调用拒绝、
200 次实例生命周期，以及一段 5,000 块的流。详见[验证记录](docs/VALIDATION.md)。

### 仓库检查

```sh
python3 tools/check_distribution.py
```

它按只含源码的路径策略和二进制限制，检查暂存内容和所有可达提交树。提交前也请自己检查实际的暂存 diff。
检查通过**不等于**法律上允许跨设备使用。

## 文档

[C API 约定](docs/API.md) ·
[可调后处理](docs/POSTPROCESS.md) ·
[验证记录](docs/VALIDATION.md) ·
[分发边界与来源说明](docs/DISTRIBUTION.md)

## 许可证

本仓库自己的代码和文档采用 [MIT License](LICENSE)。它不授予 Sony 或其他第三方程序、模型权重、参数或商标的任何权利。
你需要自行提供合法取得的文件，并自行确认适用的软件许可和当地互操作性/著作权规则，详见[分发边界](docs/DISTRIBUTION.md)。
