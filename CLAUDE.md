# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is the implementation of **"参考应用3：轻量级视频流媒体服务器"** (Reference Application 3: Lightweight Video Streaming Server) from the 睿赛德 (RT-Thread) competition. The project uses the Canaan K230 chip + RT-Thread Smart microkernel OS.

**Application scenario**: Smart door locks, baby monitors, IoT devices that push local camera feeds to a mobile app while simultaneously processing AI tasks like motion detection. If all functions run in a single process, video encoding load fluctuations degrade AI response real-time. RT-Thread Smart's multi-process model separates high-load streaming from critical event handling.

### Competition Deliverables (Minimum)

| Requirement | Detail |
|-------------|--------|
| **Process 1 — Capture & Streaming** | Capture camera frames, run a lightweight **RTSP server** to publish video stream |
| **Process 2 — AI Event** | Motion detection on the same video source; on detection, notify Process 1 via **IPC**, overlay **"Motion Detected!"** OSD on stream, save snapshot to **SD card** |
| **Performance bar** | Process 1 fully loaded at **1080P@15fps**; end-to-end delay from motion event to OSD overlay complete **< 300ms** |
| **Stability** | Both processes must never deadlock or crash |

### Current Phase

Week 1 — **single-process MVP** to validate the MPP hardware pipeline (VB pool → VI → VENC bind). Multi-process architecture and RTSP server are deferred until the underlying hardware pipeline is stable.

**Iron rule for Week 1**: No multi-process code. Everything runs in a single `main()` with multiple threads. Validate hardware logic first, then split into processes.

## Team Structure & Roles

The user is the **队长 (Team Lead)**, responsible for the MPP core pipeline. The team has 3 members:

```
[摄像头] ---> [VI] ======硬件Bind======> [VENC] ---> [NALU 压缩码流]
                ||                           ||                  |
                ||(分流低分辨率通道)          ||(硬件OSD叠加)     | (队长产出→队员A发送)
                ||                           ||                  v
         [VI_CH1 640x480]             [OSD Region]          [LwIP Socket → 网络]
                |                           ^                   ^
                v                           |                   |
         [队员B: AI算法] ---(IPC通知)---> 设置Alpha寄存器       [SD卡写盘线程]
```

| Role | Responsibility | Key Deliverables |
|------|---------------|-----------------|
| **队长 (You)** | MPP core pipeline: VB pool init, VI config, hardware Bind VI→VENC, IPC channel setup | **Week 1**: serial console prints `"Get NALU, Size: xxxx bytes"` at 15fps, runs 10 min without crash |
| 队员A | Network streaming (LwIP socket, RTSP/UDP), SD card async write (FatFS, low-priority thread) | **Week 1**: fake data UDP sender → received by PC Wireshark |
| 队员B | AI motion detection (frame differencing), hardware OSD trigger (Alpha register) | **Week 1**: C frame-diff algorithm on PC; `kd_mpi_venc_set_region` test on K230 |

### Key Architecture Concepts for Team Lead

- **MMU & Process Isolation**: RT-Smart enables MMU — each process has independent virtual address space. Passing a raw pointer between processes causes a segfault. Use IPC message queues to pass physical addresses instead.
- **VB Pool** = DMA-specialized SRAM for all peripherals. Analogous to `#pragma`-reserved DMA buffer in bare-metal STM32.
- **Hardware Bind** = STM32 timer master-slave mode triggering ADC. Once configured, peripherals transfer data autonomously — no CPU ISR needed.
- **OSD = Hardware Overlay**: NOT a CPU `for`-loop painting pixels. A pre-loaded bitmap in physical memory, controlled by a single Alpha register write. Alpha=0 invisible, Alpha=255 overlay applied at hardware level in 0ms CPU time.
- **IPC Message Queue**: The only legal cross-process data path. Analogous to SPI between two independent MCUs — send packed structs, not raw pointers.

### Week 1 Deliverables — Team Lead

1. Write VB pool init code per K230 MPP API Reference Manual
2. Configure `VI_CH0` (1080P) and `Bind` to `VENC`
3. **Acceptance**: Serial terminal prints `"Get NALU, Size: xxxx bytes"` at 15fps, stable for 10+ minutes

## Repository Structure

```
/home/ubuntu/workspace/           # Primary working directory (NOT inside SDK)
├── big_core/                     # RT-Smart big-core apps (SCons build)
│   ├── SConstruct                # BuildApplication('big_app', 'SConscript', ...)
│   ├── SConscript                # File whitelist for compilation
│   ├── cconfig.h                 # RT-Thread GCC/MUSL compiler config (auto-gen)
│   ├── hello/                    # Minimal hello-world app
│   ├── create_task/              # RT-Thread dynamic thread creation
│   ├── delete_task/              # RT-Thread thread deletion
│   ├── mutex/                    # RT-Thread mutex demo
│   ├── semaphore/                # RT-Thread semaphore demo
│   ├── queue/                    # RT-Thread message queue demo
│   ├── event_group/              # RT-Thread event flag demo
│   ├── test/                     # RT-Thread event + multi-thread demo
│   └── build/big_app/            # Build output objects
├── small_core/                   # Linux small-core apps (Makefile build)
│   ├── makefile                  # Uses Xuantie-900 glibc toolchain
│   └── hello.c
├── hello_task/                   # Standalone big-core hello (different Makefile variant)
│   ├── makefile                  # Uses riscv64-linux-musl toolchain
│   └── hello.c
├── 要求.md                       # Project specification & constraints (Chinese)
├── 安排.md                       # Team coordination guide, roles & Week 1 plan (Chinese)
└── 嵌赛睿赛德赛题.pdf            # RT-Thread competition task description (Reference App 3)
```

## SDK Location & Toolchains

```
K230 SDK:  /home/ubuntu/k230_sdk/
Big-core RT-Smart:  /home/ubuntu/k230_sdk/src/big/rt-smart/
MPP includes:       /home/ubuntu/k230_sdk/src/big/mpp/include/comm/
MPP API headers:    /home/ubuntu/k230_sdk/src/big/mpp/userapps/api/
```

**Big core toolchain (musl, used by RT-Smart kernel + big_core)**:
`/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin/riscv64-unknown-linux-musl-gcc`

**Big core toolchain (glibc, used by small_core Linux)**:
`/home/ubuntu/k230_sdk/toolchain/Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0/bin/riscv64-unknown-linux-gnu-gcc`

## Build System

### Big Core (RT-Smart, SCons-based)
The SConstruct file pins absolute paths to the SDK. `BuildApplication('big_app', 'SConscript', usr_root=USERAPPS_DIR)` uses RT-Smart's `building.py` module. The SConscript file uses a **whitelist pattern** — only explicitly listed `.c` files in the `src` array are compiled.

**CRITICAL: Build requires RTT_EXEC_PATH env var** because the SDK `.config` references `/opt/rtt/...` but the actual toolchain is under `/home/ubuntu/k230_sdk/toolchain/`:
```bash
RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin" scons -j4
```
Output is `big_core/build/big_app.elf`.

### Small Core (Linux, Makefile-based)
Standard Makefile with hardcoded toolchain path. Build:
```bash
cd small_core && make
```

## MPP Hardware Pipeline Architecture

All video processing goes through the MPP (Media Processing Platform) hardware pipeline. Data flows between modules via **hardware bind** (`kd_mpi_sys_bind`) or physical address pointer passing — **never CPU memcpy**.

### Key MPP Modules
| Module | Purpose | API Header |
|--------|---------|------------|
| VB | Video Buffer pool (physically contiguous MMZ memory) | `mpi_vb_api.h` |
| VI / VVI | (Virtual) Video Input — sensor frame capture | `mpi_vvi_api.h` |
| VENC | Video Encoder — H.264/H.265 hardware encoding | `mpi_venc_api.h` |
| SYS | System control — bind/unbind, MMZ alloc, log | `mpi_sys_api.h` |

### VB Pool Initialization Sequence
1. `kd_mpi_vb_set_config(&config)` — configure pool before init
2. `kd_mpi_vb_init()` — initialize (can be called multiple times safely)
3. `kd_mpi_vb_create_pool(&pool_config)` — create individual pools
4. `kd_mpi_vb_get_block(pool_id, blk_size, mmz_name)` — acquire a block
5. `kd_mpi_vb_release_block(block)` — return block to pool

### Key VB Config Struct (`k_vb_config`, defined in `k_vb_comm.h`)
```c
typedef struct {
    k_u32 max_pool_cnt;                              // max pools [0, VB_MAX_POOLS=256]
    k_vb_pool_config comm_pool[VB_MAX_COMM_POOLS];   // common pool configs
} k_vb_config;

typedef struct {
    k_u64 blk_size;       // size per block
    k_u32 blk_cnt;        // number of blocks
    k_vb_remap_mode mode; // NONE/NOCACHE/CACHED
    char mmz_name[16];    // MMZ zone name (empty = anonymous)
} k_vb_pool_config;
```

### Hardware Bind
`kd_mpi_sys_bind(src_chn, dest_chn)` — binds a source channel to a destination channel at hardware level. Data flows automatically without CPU involvement. Both parameters are `k_mpp_chn*`.

### VENC Sequence
1. `kd_mpi_venc_create_chn(chn_num, &attr)`
2. `kd_mpi_venc_start_chn(chn_num)`
3. `kd_mpi_venc_send_frame(chn_num, &frame, timeout)` — or receive via bind
4. `kd_mpi_venc_get_stream(chn_num, &stream, timeout)` — get encoded NALU
5. `kd_mpi_venc_release_stream(chn_num, &stream)`

## Critical Hardware Constraints

These are hard requirements from the K230 MPP architecture — violating them causes system crashes:

1. **No V4L2 or generic Linux multimedia APIs** — must use K230 native MPP APIs exclusively
2. **No CPU frame copy** — all frame data flow via `kd_mpi_sys_bind` hardware binding or physical address passing; never `malloc`/`memcpy`/`mmap` on video frames
3. **No ffmpeg/gstreamer** — NALU extraction must use `kd_mpi_venc_get_stream` directly
4. **No software OSD** — OSD must use VENC hardware Region functions (dynamic Alpha register); never CPU pixel iteration

### Memory Allocation Rule
- **1080P NV12 one frame**: 1920 × 1080 × 1.5 = 3,110,400 bytes ≈ 3.0 MiB
- VB Pool must allocate enough blocks for pipeline depth (VI capture queue + VENC reference frames + user hold buffers)

## RT-Thread Smart Programming Model

- Use `rt_thread_create()` + `rt_thread_startup()` for dynamic threads
- IPC primitives: `rt_mutex`, `rt_semaphore`, `rt_event`, `rt_messagequeue`
- Thread priority and stack size must be explicitly set
- Main thread runs an idle loop (`while(1) rt_thread_mdelay(100)`) to prevent exit
- All RT-Thread headers are in the RT-Smart SDK userapps path

## Error Handling Convention

All MPP APIs return `k_s32`. Use macros from SDK headers (`K_ERR_XXX` pattern):
```c
#define K_ERR_VB_NOMEM     K_DEF_ERR(K_ID_VB, K_ERR_LEVEL_ERROR, K_ERR_NOMEM)
#define K_ERR_VB_UNEXIST   K_DEF_ERR(K_ID_VB, K_ERR_LEVEL_ERROR, K_ERR_UNEXIST)
// ... etc. Check each API's return value against 0 (success) and handle accordingly.
```

## Before Writing MPP Code

Always read the relevant SDK headers first — struct member names and macro values are the authoritative reference. Key headers:

- `/home/ubuntu/k230_sdk/src/big/mpp/include/comm/k_vb_comm.h` — VB config structs, pool limits, VB_UID enum
- `/home/ubuntu/k230_sdk/src/big/mpp/include/comm/k_video_comm.h` — frame info, pixel formats
- `/home/ubuntu/k230_sdk/src/big/mpp/include/comm/k_sys_comm.h` — MPP channel struct, bind definitions
- `/home/ubuntu/k230_sdk/src/big/mpp/include/comm/k_venc_comm.h` — VENC channel attr, stream structs
- `/home/ubuntu/k230_sdk/src/big/mpp/userapps/api/mpi_vb_api.h`
- `/home/ubuntu/k230_sdk/src/big/mpp/userapps/api/mpi_sys_api.h`
- `/home/ubuntu/k230_sdk/src/big/mpp/userapps/api/mpi_venc_api.h`
- `/home/ubuntu/k230_sdk/src/big/mpp/userapps/api/mpi_vvi_api.h`
