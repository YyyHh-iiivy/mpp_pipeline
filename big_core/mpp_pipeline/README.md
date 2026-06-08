# K230 MPP Pipeline 模块说明

本目录是 Week 1 队长侧的单进程 MPP Pipeline MVP，用于验证 K230 硬件视频链路是否稳定：

```text
GC2093 Sensor -> VICAP/VI -> kd_mpi_sys_bind -> VENC(H.265) -> stream_thread 打印 NALU
```

当前拆分只是**源码模块化**，不是多进程改造：

- 输出仍然是一个 `big_app.elf`
- 运行时仍然是一个进程
- 仍然只有 `main` 线程 + `stream_thread` 码流线程
- Week 1 不引入 IPC、RTSP、OSD、AI 或大小核协作代码

---

## 数据流

```text
┌──────────────────────────────────────────────┐
│ GC2093 摄像头                                │
│ 1920x1080, MIPI CSI, 30fps                   │
└──────────────────────┬───────────────────────┘
                       │
┌──────────────────────▼───────────────────────┐
│ VICAP / VI                                   │
│ - ONLINE 模式                                │
│ - NV12 输出                                  │
│ - 1080P 通道                                 │
└──────────────────────┬───────────────────────┘
                       │ kd_mpi_sys_bind()
                       │ 硬件绑定，CPU 零拷贝
┌──────────────────────▼───────────────────────┐
│ VENC                                         │
│ - H.265 Main Profile                         │
│ - CBR 4000kbps                               │
│ - 获取编码后 NALU                            │
└──────────────────────┬───────────────────────┘
                       │ kd_mpi_venc_get_stream()
┌──────────────────────▼───────────────────────┐
│ stream_thread                                │
│ - 打印 Get NALU, Size: xxxx bytes            │
│ - 每 150 帧打印一次统计信息                  │
└──────────────────────────────────────────────┘
```

---

## 文件职责

| 文件 | 职责 |
|------|------|
| `mpp_pipeline.c` | 程序入口、全局状态定义、信号处理、初始化编排、600 秒自动退出、等待码流线程、最终 PASS/FAIL 日志 |
| `mpp_pipeline.h` | 公共配置、SDK 头文件、通道 ID、buffer size 宏、状态机、日志宏、函数声明 |
| `vb_module.c` | VB pool 配置和初始化 |
| `venc_module.c` | VENC 通道创建/启动，以及从 VENC 获取 NALU 的 `stream_thread` |
| `vicap_module.c` | 传感器信息查询、VICAP device/channel 配置、GC2093 CSI fallback、启动 VICAP stream |
| `bind_module.c` | VI/VICAP 到 VENC 的硬件绑定 |
| `cleanup_module.c` | 按状态机逆序释放 MPP 资源 |
| `SConscript` | 当前目录的源码白名单、MPP include/lib 配置 |

---

## 初始化顺序

初始化顺序不要随意调整。当前顺序为：

1. `vb_init()`
   - 配置并初始化 VB pool
2. `venc_init(VENC_CHN, ENC_BITRATE)`
   - 创建并启动 VENC 编码通道
3. `vicap_config(SENSOR_TYPE)`
   - 查询传感器并配置 VICAP device/channel
4. `vi_bind_venc()`
   - 调用 `kd_mpi_sys_bind()` 建立硬件绑定
5. `vicap_start()`
   - 启动 VICAP stream
6. `pthread_create(..., stream_thread, ...)`
   - 创建码流采集线程
7. `main()` 等待 Ctrl+C 或 600 秒自动退出

状态机 `pipeline_status` 用于记录已经完成到哪一步，清理时根据状态跳过未初始化资源。

---

## 清理顺序

清理顺序必须和初始化相反，不能随意调整。当前顺序为：

1. `kd_mpi_vicap_stop_stream(VICAP_DEV)`
2. `kd_mpi_sys_unbind(&vi, &venc)`
3. `kd_mpi_vicap_deinit(VICAP_DEV)`
4. `kd_mpi_venc_stop_chn(VENC_CHN)`
5. `kd_mpi_venc_destroy_chn(VENC_CHN)`
6. `kd_mpi_venc_close_fd()`
7. `kd_mpi_vb_exit()`

> 注意：MPP 硬件资源释放顺序不正确可能导致卡死、崩溃或下次运行初始化失败。修改清理逻辑后必须重新做板端稳定性测试。

---

## 构建方法

在开发机上从 `big_core` 目录构建：

```bash
cd /home/ubuntu/workspace/big_core
RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin" scons -j4
```

预期输出：

```text
/home/ubuntu/workspace/big_core/build/big_app.elf
```

本工程使用 SCons 源码白名单。新增 `.c` 文件后必须同步加入：

- `big_core/SConscript`
- `big_core/mpp_pipeline/SConscript`

不要改成 `Glob('*.c')`，避免临时文件或实验文件被意外编译进正式 ELF。

---

## 板端验证

把 `big_app.elf` 放到 K230 板端后运行：

```bash
./big_app.elf
```

短测时应看到：

```text
[MPP] VB pool init OK
[MPP] VENC chn=0 init OK
[MPP] VICAP dev=0 chn=0 init OK
[MPP] VI->VENC bind OK
[MPP] VICAP stream started
[MPP] Get NALU, Size: xxxx bytes  [frame #N]
```

完整稳定性测试应看到：

```text
[MPP] Auto-exit after 600 seconds.
[MPP] Stream thread exit. Total: ... in 600.0s (... fps)
[MPP] Cleanup complete
[MPP] Pipeline test PASSED.
```

最近一次拆分前的验收结果为：连续运行 600 秒，输出 18000 帧，平均约 30fps，自动退出并清理成功。

---

## 修改守则

为了保护已经跑通的硬件 Pipeline，后续修改请遵守：

1. Week 1 不加入多进程代码。
2. 不使用 V4L2、ffmpeg、gstreamer 等通用多媒体 API。
3. 视频帧不做 CPU `memcpy`，数据流仍通过 MPP 硬件绑定或物理地址机制。
4. 不随意修改 VB/VICAP/VENC 参数，修改后必须重新上板测试。
5. 不随意修改清理顺序。
6. 不把硬件 OSD 做成 CPU 像素循环。
7. 保持 SConscript 显式源码白名单。
8. 纯重构时尽量保持日志格式不变，便于对比测试结果。
