# Repository Guidelines

## Project Structure & Module Organization

This repository contains K230/RT-Thread Smart C examples and the Week 1 MPP pipeline MVP.

- `big_core/`: RT-Smart big-core applications built with SCons. The active app is assembled through `big_core/SConstruct` and `big_core/SConscript`.
- `big_core/mpp_pipeline/`: modular MPP pipeline source: VB, VICAP/VI, VENC, bind, cleanup, and shared header files.
- `big_core/{hello,create_task,delete_task,mutex,semaphore,queue,event_group,test}/`: focused RT-Thread API demos.
- `small_core/`: migrated Linux small-core RTSP/RTP sender, DATAFIFO bridge, headers, tests, notes, and downloadable executables. This workspace copy is the sole maintained source and builds with the Xuantie glibc toolchain; selected outputs under `small_core/user/` are versioned for direct GitHub download.
- `hello_task/`: standalone big-core hello example built with a Makefile and musl toolchain.
- `*.md` and `嵌赛睿赛德赛题.pdf`: project notes, requirements, logs, and competition reference material.

Build outputs such as `build/`, `*.o`, and `*.elf` are generated artifacts; avoid changing them unless the task explicitly requires refreshed binaries.

## Build, Test, and Development Commands

Run big-core builds from `big_core/`:

```bash
RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin" scons -j4
```

This produces `big_core/build/big_app.elf`. SCons uses explicit source whitelists; add new `.c` files to the relevant `SConscript`.

Build examples:

```bash
cd small_core && make test && make build && make verify
cd hello_task && make
```

Clean Makefile examples with `make clean`. For SCons, use `scons -c` from `big_core/`.

## Coding Style & Naming Conventions

Use C with 4-space indentation and existing brace style. Keep module-level functions grouped by hardware responsibility, for example `vb_init`, `venc_init`, `vicap_start`, and `pipeline_cleanup`. Prefer descriptive constants in headers over magic numbers. Check every MPP API return value (`k_s32`) and log failures with existing `[MPP]` or `[ERROR]` patterns.

Do not replace SConscript whitelists with broad globs. Do not introduce V4L2, ffmpeg, gstreamer, CPU frame copies, or software OSD paths for the MPP pipeline.

## Agent-Specific Instructions

与用户的所有对话、状态更新和最终回复必须使用中文，除非用户明确要求其他语言。代码、命令、文件路径和日志保持原文。

同一 Bug、功能或重构的连续修改期间，不要在每次编辑代码文件后立即重复更新项目根目录下的 `程序流程.md`。应在以下逻辑检查点统一同步：

- Bug 或功能完成并准备交付。
- 新增或删除模块。
- 函数签名、调用链、模块关系或数据流完成重大调整。
- 准备提交、合并或移交工作。

仅修改注释、日志文本、测试规则或临时诊断代码，且项目架构和数据流未变化时，可以不更新 `程序流程.md`。任务被撤销或代码已回退时，不记录临时方案。

`程序流程.md` 用于帮助用户和队友理解项目架构与流程，应使用“函数抽象 + 调用链 + 数据追踪”的方式编写。同步时必须确保：

- 新增的文件和函数被及时抽象加入。
- 修改的函数签名、调用关系被正确反映。
- 删除的内容被清理。
- 整体结构保持清晰可读。

最终交付前必须检查 `程序流程.md` 与代码一致。本次任务实际发生文档同步时，回复 `已同步更新程序流程.md` 作为确认。

## Testing Guidelines

There is no automated unit test framework. Verification is build success plus board-side runtime validation. For `big_core/mpp_pipeline`, copy `big_app.elf` to the K230 board and run it. Expected logs include VB/VENC/VICAP init success, `VI->VENC bind OK`, repeated `Get NALU, Size: xxxx bytes`, clean auto-exit after 600 seconds, and `Pipeline test PASSED`.

After changing VB, VICAP, VENC, bind, or cleanup logic, repeat the full stability run.

## Commit & Pull Request Guidelines

Current history uses concise Chinese commit summaries describing completed work. Follow that style, for example: `完成VENC清理顺序修复` or `补充MPP管线构建说明`.

Pull requests should include: purpose, touched modules, build command output summary, board validation result when hardware behavior changes, and any relevant terminal logs or screenshots.
