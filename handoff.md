# MPP 低延迟优化 Handoff

## 1. 文档用途

本文是后续多轮对话的持续交接入口。开始新对话时先阅读本文、`程序流程.md` 和当前 `git diff`；形成新的板端证据、架构决策或优化结果后同步更新本文，避免重复推理或把旧实验结论误当成当前事实。

## 2. 当前基线

- 稳定基线提交：`e16e5bb`（`移除主动idr`）。
- 当前优化分支：`后期针对运动检测优化延迟`。
- 主链路：`GC2093 -> VICAP 1920x1080 -> VI/VENC bind -> H.265 VENC -> DATAFIFO -> 小核 RTSP/RTP`。
- AI 旁路：`VICAP channel 2 640x480 NV12-Y -> AI motion thread -> OSD + snapshot request`。
- 主视频目标：`1080p@15fps`，端到端延迟稳定 `<300ms`。
- AI 当前稳定版本：取帧前限频，实际约 `4.6~4.7fps`，保留连续 3 帧确认。
- 最新板测：连续约 742 秒，端到端延迟约 `240ms`；AI `dump_fps/process_fps=4.6~4.7`，`dump_fail=0`、`release_fail=0`，主 VENC 约 `14.8fps`，pending 稳定，无持续积压。
- 最新问题：首次持续运动约 2 秒仍不容易触发，尚未证明是采样间隔、连续确认还是状态机复位问题。

## 3. 已确认事实

1. 用户在网络、播放器和场景条件一致时进行了多轮 A/B 测试；关闭运动检测线程后，端到端延迟显著降低。
2. 历史分支已有“保留 AI VICAP 通道、只关闭 AI 用户态线程”的单变量实验，低延迟和大幅运动稳定性恢复，说明 AI 通道配置本身不是唯一解释。
3. 历史上关闭 OSD 后问题仍存在，因此 OSD 显隐不是当前首要嫌疑。
4. 当前稳定基线已经移除快照触发的主动 IDR；旧文档中的“强制 IDR 导致突发码流”不能继续作为当前 HEAD 的主要解释。
5. 优化前 AI 成功路径没有主动限频：`dump -> mmap -> detect -> munmap -> release -> 立即下一帧`，板端历史日志曾显示实际约 28.6fps。
6. 每个成功帧都会 `calloc/free` handle、`mmap/munmap` Y 平面、两遍抽样扫描并完整复制约 300KB 的上一帧 Y 数据。

## 4. 当前因果判断

结论：运动检测线程造成主视频延迟的假设具有较强的准因果证据，可以作为首要优化依据；但尚未证明线程内部哪一段是主导成本。

按当前证据排序的候选机制：

1. 高频 `kd_mpi_vicap_dump_frame()` 与 dump buffer 生命周期竞争。
2. 每帧 `mmap/munmap` 和非缓存 Y 平面读取产生的系统调用及 DDR 压力。
3. 完整 `640x480` 参考帧复制带来的额外内存流量。
4. 两遍抽样和直方图运动算法的 CPU 开销。
5. 运动事件后的 OSD/快照路径；当前优先级较低，但仍需板端观测。

不能把“主 VENC 仍为 15fps”直接等价为“主链路没有延迟”：实时流可能保持输出帧率，但帧年龄或下游队列仍持续增长。

## 5. 已批准并实施的首轮决策

- 用户选择直接恢复 5fps 取帧前限频，不先做分段耗时插桩。
- 使用 `CLOCK_MONOTONIC` 控制节奏。
- 首帧可立即取；后续从 dump 失败或成功帧释放完成时重新等待 200ms。
- 不在旧 deadline 上累加，防止处理超时后连续追帧。
- 不在持有 dump 帧期间 sleep。
- 每个成功获取的帧仍执行一次检测，不使用“先高速 dump、再跳过算法”的方式。
- 保留连续 3 帧确认、自适应阈值、OSD、自然 IDR 快照、线程优先级和 DATAFIFO 协议。
- 每 5 秒打印一次 `[health:ai]`；线程退出时打印实际 dump/process FPS。

## 6. 当前诊断计划

- 默认性能构建保持 `MOTION_STATE_DIAG_ENABLE=0`、`AI_MOTION_ACQUIRE_FPS=5`。
- 诊断构建通过 SCons 参数开启：`MOTION_STATE_DIAG_ENABLE=1`；FPS 只允许选择 `5` 或 `10`，间隔分别为 `200ms` 或 `100ms`。
- 先做 5fps 短测：`静止5秒 -> 持续运动2秒 -> 静止5秒 -> 持续运动2秒`，每个样本记录 `frame/score/is_motion/hit/clear/latched/reason`。
- 只有 `confirm_wait` 被 `no_motion` 反复清零时才进入 10fps；若 `latched/clear_wait` 主导，则停止升频，重新设计锁存复位策略。
- 10fps 灵敏度测试重复 10 次，至少 9 次触发才算恢复；随后关闭详细诊断运行 10 分钟，要求实际 AI 约 `9~10fps` 且主视频 `<300ms`。

## 7. 诊断构建命令

当前待上板产物：`big_core/big_app.elf`，配置为 `AI_MOTION_ACQUIRE_FPS=5`、`MOTION_STATE_DIAG_ENABLE=1`，SHA256：

```text
5bcc5b3f61f0ad9d3f663a8cfd2f91e1785f34d100f9f54d6c8af0fae35b451e
```

```bash
cd /home/ubuntu/workspace/big_core
RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin" \
  scons -c && \
RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin" \
  scons -j4 MOTION_STATE_DIAG_ENABLE=1 AI_MOTION_ACQUIRE_FPS=5
sha256sum big_app.elf
```

10fps 诊断只把最后的 FPS 参数改成 `AI_MOTION_ACQUIRE_FPS=10`；性能验收省略 `MOTION_STATE_DIAG_ENABLE=1`。每次换变体先 `scons -c` 并保存 SHA256 与启动指纹。

## 8. 板端验证矩阵

所有 A/B 测试必须固定摄像头、网络、播放器参数、小核程序和测量方法。

| 场景 | 至少时长 | 重点观察 |
| --- | ---: | --- |
| 静止画面 | 2 分钟 | 基础延迟、AI FPS、VENC FPS、pending |
| 小范围运动 | 2 分钟 | 事件响应、OSD、快照、延迟波动 |
| 持续运动 | 2 分钟 | 是否积压、停止运动后延迟是否恢复 |
| 大面积剧烈运动 | 2 分钟 | 卡顿、NALU 峰值、DATAFIFO drop/pending |
| 完整稳定性 | 至少 600 秒 | 无死机、无资源泄漏、退出清理完整 |

验收条件：

- 各场景端到端延迟稳定 `<300ms`，不随运行时间持续增长。
- 5fps 版本 `[health:ai] dump_fps/process_fps` 正常约为 `4~5fps`；10fps 版本约为 `9~10fps`，不得明显高于所选上限持续运行。
- 正常运行时 `dump_fail`、`release_fail` 不持续增加，`last_frame_age_ms` 不异常增长。
- 主 VENC 维持约 15fps，DATAFIFO pending 不长期占满。
- 5fps 连续运动理论响应约 `400~600ms`；10fps 试验要求 10 次规定动作至少 9 次触发，OSD 和快照路径仍工作。
- Ctrl+C 后出现完整 cleanup 和 `Pipeline test PASSED.`。

需要保存的板端日志：

- `AI motion acquire config` 和 `[health:ai]`。
- stream FPS、码率、`pending=`。
- DATAFIFO drop/full、`seq_gap`、`read_done_age_ms`。
- 运动事件、OSD、快照入队和快照标志投递。
- cleanup 全流程。

## 9. 结果解释

- 5fps 后延迟稳定恢复：确认主要因果路径位于高频 AI 帧生命周期及其共享资源竞争；下一轮继续细分映射、内存访问和算法成本。
- AI FPS 已降至 5 但延迟仍高：优先检查单次 dump/mmap/release 的长尾、事件触发链和小核发送队列帧年龄。
- 静止正常但大幅运动积压：重点检查 VENC 大帧、DATAFIFO pending、网络发送队列和小核“保最新帧”策略。
- `[health:ai]` 明显高于所选 FPS：限频没有按当前 ELF 生效，先核对 ELF 的 SHA256、启动指纹和源码提交，禁止继续混合版本分析。

## 10. 后续优化顺序

1. 完成 5fps 版本板测并把实测值填回本文。
2. 将上一帧缓存从完整 `640x480` 改成实际采样使用的 `160x120`，避免每帧完整复制 Y 平面。
3. 消除每帧 `calloc/free`，优先使用线程栈 handle；保持一帧一次 acquire/release 所有权。
4. 查阅 K230 MPP 文档确认 dump buffer 生命周期后，再评估是否允许复用映射；未经确认不得跨帧持有物理地址或映射。
5. 将 OSD 自动隐藏从 AI 取帧节奏中解耦，并评估用有界事件队列隔离检测与副作用。
6. 评估 stream 线程先于 AI 线程启动，避免早期运动事件先于码流消费者出现。
7. 联合小核检查 RTSP/RTP 发送和快照队列，拥塞时采用“保最新可解码帧”而不是累计历史帧。

## 11. 待讨论的架构问题

- AI 旁路是否需要继续保持 `640x480`，还是可在不损失检出率的前提下降到更小分辨率。
- 5fps 是否满足最终比赛场景；板测稳定后再决定是否尝试 8fps 或 10fps。
- `motion_adapter` 的连续帧确认是否应改为基于时间，以避免检测 FPS 改变事件语义。
- OSD deadline 应归属独立控制模块还是 stream 线程安全点。
- 快照等待超过 `VENC_GOP + 2` 后标记非 Header stream 是否满足小核快照解码契约。
- cleanup 状态是否需要覆盖 AI、OSD、export 和 stream thread 的独立初始化状态。

## 12. 下一次对话入口

下一次优先提供：5fps 逐帧诊断日志、板端实际运行 ELF 的 SHA256、规定动作的触发次数，以及 `[health:ai]`、stream `pending=`、DATAFIFO age/drop 指标。先按 `reason` 判断是确认计数被清零还是锁存阻止，再决定是否进入 10fps A/B。
