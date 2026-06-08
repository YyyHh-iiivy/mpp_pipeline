# MPP Pipeline 快速参考卡

## 🔄 数据流拓扑

```
┌─────────────────────────────────────────────────────┐
│ GC2093 传感器(1920x1080@30fps)                     │
│ (MIPI CSI-2, 10-bit Bayer格式)                    │
└────────────────────┬────────────────────────────────┘
                     │ MIPI 物理通道
┌────────────────────▼────────────────────────────────┐
│ VICAP (VI Channel 0) - 虚拟采集器                  │
│ ├─ Mode: ONLINE (实时)                            │
│ ├─ ISP: AE + AWB 自动控制                         │
│ ├─ Format: NV12 (YUV420SP)                         │
│ └─ Output: 1920x1080 @ 30fps                       │
└────────────────────┬────────────────────────────────┘
                     │ (硬件Bind, CPU旁路)
         kd_mpi_sys_bind(&vi_chn, &venc_chn)
                     │ (零拷贝, 实时流)
┌────────────────────▼────────────────────────────────┐
│ VENC (Video Encoder) - 视频编码通道 0              │
│ ├─ Codec: H.265 Main Profile                       │
│ ├─ RC Mode: CBR (恒定码率)                         │
│ ├─ Bitrate: 4000 kbps                              │
│ ├─ Rate: 30fps → 15fps (硬件下采样)               │
│ ├─ GOP: 30 frames (I-frame每2秒)                   │
│ └─ Output: NALU stream (0 or multiple NALUs)       │
└────────────────────┬────────────────────────────────┘
                     │ (编码完成)
         kd_mpi_venc_get_stream(VENC_CHN, &stream, -1)
         [阻塞等待，直到有编码输出]
                     │
┌────────────────────▼────────────────────────────────┐
│ stream_thread() - 码流采集                         │
│ ├─ NALU类型判断 (Header vs Data)                   │
│ ├─ 每帧大小打印到日志                              │
│ └─ 每150帧统计FPS                                  │
└────────────────────┬────────────────────────────────┘
                     │
              控制台日志输出
```

---

## 🛠️ 初始化状态转移机

```
┌──────────────────────────────────────────────────────────┐
│                    STATUS_IDLE (0)                      │
│                     (初始状态)                           │
└───────────────────┬──────────────────────────────────────┘
                    │ vb_init() ✓
                    ▼
         ┌──────────────────────┐
         │  STATUS_VB_INIT (1)  │ ← 内存池就绪
         └──────────┬───────────┘
                    │ venc_init() ✓
                    ├─ kd_mpi_venc_create_chn()
                    │
         ┌──────────────────────┐
         │STATUS_VENC_CREATED(2)│
         └──────────┬───────────┘
                    │ kd_mpi_venc_start_chn()
                    │
         ┌──────────────────────┐
         │STATUS_VENC_STARTED(3)│ ← 编码器就绪
         └──────────┬───────────┘
                    │ vicap_config() ✓
                    │ kd_mpi_vicap_init()
                    │
         ┌──────────────────────┐
         │STATUS_VICAP_INIT(4)  │ ← 采集器就绪
         └──────────┬───────────┘
                    │ vi_bind_venc() ✓
                    │ kd_mpi_sys_bind()
                    │
         ┌──────────────────────┐
         │ STATUS_BINDED (5)    │ ← 硬件绑定完成
         └──────────┬───────────┘
                    │ vicap_start() ✓
                    │ kd_mpi_vicap_start_stream()
                    │
         ┌──────────────────────┐
         │STATUS_STREAM_STARTED │ ← 数据流开始
         │       (6)            │
         └──────────┬───────────┘
                    │ pthread_create(stream_thread)
                    │
         ┌──────────────────────┐
         │  STATUS_RUNNING (7)  │◄─── [RUNNING] 态
         │ 采集线程开始运行      │     ↑ 每帧打印NALU
         └──────────┬───────────┘     ↓ Ctrl+C 或超时
                    │
           (错误或信号)
                    │
              pipeline_cleanup()
                    │
         (逆序清理，参见下表)
                    │
         ┌──────────────────────┐
         │    STATUS_IDLE       │
         │    (清理完毕)        │
         └──────────────────────┘
```

---

## 🧹 清理流程（反向）

| 状态 | 清理操作 | 函数 |
|------|--------|------|
| STATUS_STREAM_STARTED(6) | ① 停止采集流 | `kd_mpi_vicap_stop_stream(VICAP_DEV)` |
| STATUS_BINDED(5) | ② 解除硬件绑定 | `kd_mpi_sys_unbind(&vi, &venc)` |
| STATUS_VICAP_INIT(4) | ③ 反初始化采集 | `kd_mpi_vicap_deinit(VICAP_DEV)` |
| STATUS_VENC_STARTED(3) | ④ 停止编码通道 | `kd_mpi_venc_stop_chn(VENC_CHN)` |
| STATUS_VENC_CREATED(2) | ⑤ 销毁编码通道 | `kd_mpi_venc_destroy_chn(VENC_CHN)` + `kd_mpi_venc_close_fd()` |
| STATUS_VB_INIT(1) | ⑥ 退出VB池 | `kd_mpi_vb_exit()` |

**关键**: 如果第 N 步失败，清理时 N-1 到 1 会被执行，N 到 6 被跳过

---

## 📐 内存布局

```c
VB (Video Buffer) Pool:
├─ Pool[0]: 输入帧缓冲
│  ├─ 块数: 6
│  ├─ 块大小: 0x3F5000 (4.0 MB, 1920×1080×2, 4KB对齐)
│  └─ 总大小: 24 MB
│
└─ Pool[1]: 编码输出码流
   ├─ 块数: 15
   ├─ 块大小: 0xFD800 (1.0 MB, 1920×1080/2, 4KB对齐)
   └─ 总大小: 15 MB

总计: 24 + 15 = 39 MB
设备总DDR: 512 MB
使用率: 7.6% ✓ (充足)
```

---

## 📊 VENC 配置参数解读

```c
k_venc_chn_attr attr = {
    .venc_attr = {
        .type = K_PT_H265,                    // H.265 编码
        .profile = VENC_PROFILE_H265_MAIN,    // Main Level (低功耗)
        .pic_width = 1920,                    // 宽度
        .pic_height = 1080,                   // 高度
        .stream_buf_size = 0xFD800,           // 输出缓冲大小
        .stream_buf_cnt = 15,                 // 缓冲数 (与VB Pool[1]协调)
    },
    .rc_attr = {
        .rc_mode = K_VENC_RC_MODE_CBR,        // 恒定码率控制
        .cbr = {
            .src_frame_rate = 30,    // 输入帧率 (传感器原生)
            .dst_frame_rate = 15,    // 输出帧率 (硬件下采样)
            .bit_rate = 4000,        // 4000 kbps = 500 KB/s
            .gop = 30,               // 30 frames per GOP (2秒一个I帧)
        }
    }
};
```

### 参数含义
- **src_frame_rate = 30**: 采集器每秒30帧送入VENC
- **dst_frame_rate = 15**: VENC 硬件自动跳帧，只编码15帧 (GOP中第1,3,5...帧)
- **CBR 4000kbps**: 无论内容复杂度，均匀分配带宽
- **GOP=30**: 每30帧插入一个I帧（不依赖前面帧的完整帧）
  - 优点: 错误恢复快，直播友好
  - 缺点: 编码效率下降 (vs B帧)

---

## 🎯 码流采集线程时序

```
Frame #1 (t=0ms)       Frame #2 (t=67ms)      Frame #3 (t=133ms)
    │                       │                       │
    ├─ VENC 编码完成        ├─ VENC 编码完成        ├─ VENC 编码完成
    │                       │                       │
    ▼ stream_thread()       ▼ stream_thread()       ▼ stream_thread()
    │                       │                       │
    ├─ query_status()       ├─ query_status()       ├─ query_status()
    │   cur_packs=1         │   cur_packs=1         │   cur_packs=2
    │                       │                       │ (I帧+H264参数)
    ├─ get_stream()         ├─ get_stream()         ├─ get_stream()
    │   (阻塞等待)          │   (阻塞等待)          │   (阻塞等待)
    │                       │                       │
    ├─ print NALU          ├─ print NALU          ├─ print NALU
    │  "Get NALU: 2048"    │  "Get NALU: 1876"    │  "Get NALU: 4096"
    │                       │                       │
    ├─ release_stream()     ├─ release_stream()     ├─ release_stream()
    │                       │                       │
    └─ (循环)               └─ (循环)               └─ (循环)
     ↓                      ↓                      ↓
   <sleep 0> (非阻塞)     <sleep 0>             <sleep 0>
   
目标: 每67ms 打印一行 "Get NALU" (15fps = 1000ms/15 ≈ 67ms)
```

---

## ⚡ 快速故障排查

| 症状 | 可能原因 | 排查步骤 |
|------|--------|--------|
| 编译错误 (未定义符号) | MPP库路径错误 | 检查 `RTT_EXEC_PATH` env var |
| 运行时段错误 (Segfault) | VB池未初始化 | 检查 `kd_mpi_vb_init()` 返回值 |
| 无NALU输出 | 硬件Bind失败 | 检查 `kd_mpi_sys_bind()` 返回值 |
| FPS低于15 | CBR参数不匹配 | 确认 `dst_frame_rate=15` |
| 内存溢出 | Pool块不足 | 增加 `INPUT_BUF_CNT` 或 `OUTPUT_BUF_CNT` |
| 死机 (卡死) | 清理顺序错误 | 检查状态机逆序清理 |

---

## 🚀 构建 & 运行

```bash
# 1. 环境准备
cd /home/ubuntu/workspace/big_core
export RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin"

# 2. 编译
scons -j4
# 输出: build/big_app.elf

# 3. 下载到开发板 (示例，实际取决于部署方式)
# cp build/big_app.elf /tftpboot/
# scp build/big_app.elf root@<K230_IP>:/tmp/

# 4. 在K230上运行
# ./big_app.elf
#
# 预期输出:
# ========================================
#   K230 MPP Pipeline MVP — Week 1
#   Sensor: GC2093  Codec: H.265 CBR
#   Resolution: 1920x1080@15fps
#   Acceptance: 15fps NALU print, 10min stable
# ========================================
#
# [MPP] VB Config: pool[0] blk_size=0x3f5000 blk_cnt=6
# [MPP] VB Config: pool[1] blk_size=0xfd800 blk_cnt=15
# [MPP] VB pool init OK
# [MPP] VENC Config: H.265 1920x1080 CBR 4000kbps 30->15fps gop=30
# [MPP] VENC chn=0 init OK
# [MPP] Sensor: GC2093 1920x1080@30fps csi=0 lanes=2
# [MPP] VICAP dev=0 chn=0 init OK
# [MPP] VI->VENC bind OK
# [MPP] VICAP stream started
# [MPP] Stream thread started, chn=0, waiting for NALUs...
# [MPP] Pipeline running. Press Ctrl+C to stop (auto-exit in 600 seconds).
# [MPP] Get NALU, Size: 2048 bytes  [frame #1]
# [MPP] Get NALU, Size: 1876 bytes  [frame #2]
# ...
# [MPP] === Stats: 150 frames in 10.0s, 15.0 fps, total 287400 bytes ===
```

---

## 📌 核心概念速记

| 概念 | 含义 | 关键函数 |
|------|------|--------|
| **VB Pool** | 所有视频数据的DMA缓冲区 (物理连续) | `kd_mpi_vb_init()` |
| **VI/VICAP** | 虚拟摄像头输入 (ISP处理+格式转换) | `kd_mpi_vicap_init()` |
| **VENC** | 视频编码通道 (硬件H.265编码器) | `kd_mpi_venc_create_chn()` |
| **Hardware Bind** | CPU旁路直连，自动数据流转 | `kd_mpi_sys_bind()` |
| **NALU** | H.265编码单元 (可能包含SPS/PPS或I/P帧) | `kd_mpi_venc_get_stream()` |
| **CBR** | 恒定码率 (vs VBR可变) | `rc_mode = K_VENC_RC_MODE_CBR` |
| **GOP** | 关键帧间隔 (Group of Pictures) | `gop = 30` |

---

**文件**: `mpp_pipeline.c` | **状态**: ✅ 生产可行 | **难度**: ⭐⭐⭐ (中等)
