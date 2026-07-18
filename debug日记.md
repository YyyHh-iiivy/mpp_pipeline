# MPP 低延迟推流 Debug 日记

最后更新：2026-07-18

## 1. 记录规则与调试目标

本日记用于保存可复核的问题、证据、实验变量、结论和修正记录。事实必须能由源码、构建结果或板端日志验证；推测一律标为“假设”。后续结论变化时不删除原实验，而是在原条目后补充修正说明。

当前目标：在约 300ms 的低延迟基线上按“先 AI、后 OSD”恢复旁路运动检测和硬件 OSD；主 `VICAP -> VENC -> DATAFIFO` 链路、RTP 参数及快照协议保持不变。

固定约束：

- 主视频保持 1920×1080、CBR 和硬件 MPP bind；不引入 V4L2、ffmpeg、gstreamer、CPU 视频帧拷贝或软件 OSD。
- `SRC_FPS=15`、`DST_FPS=15`、`VICAP_OUTPUT_FPS=15`、`ENC_BITRATE=1500kbps`、`VENC_GOP=8`。
- 默认 `AI_BRANCH_ENABLE=1`，创建 AI VB pool、配置 VICAP 通道2并启动运动线程；仍可显式传入 `-DAI_BRANCH_ENABLE=0` 生成 AI-off 对照 ELF。
- 主动 IDR 默认关闭；不因运动事件或 PTS 重锁调用主动 IDR。
- 不扩大 DATAFIFO、VENC 或 UDP 缓冲来掩盖积压，不加入 VENC 自动重启。
- 快照继续使用 DATAFIFO `reserved` bit0；不修改跨核协议及快照标志语义。
- 默认 `VENC_OSD_ENABLE=1`，使用启动前一次性 region 配置、运行期 buffer 更新和 cache flush 的安全实现；仍可显式传入 `-DVENC_OSD_ENABLE=0` 复现 no-OSD A/B 对照。

### 2026-07-18：分阶段恢复 AI、硬件 OSD 与运动功能

1. 阶段一代码基线已恢复 `AI_BRANCH_ENABLE=1`：VB pool index 2、VICAP `CHN_ID_2` 的 `640x480` 旁路、`ai_motion_thread` 和 `motion_adapter` 均重新进入默认 ELF；主视频仍为 1080p/15fps/1500kbps/GOP=8。
2. 阶段二代码基线已恢复 `VENC_OSD_ENABLE=1`：`osd_init()` 只在 VENC 启动前 attach 并配置一次 region，运行期只复制/清零 ARGB buffer 并 flush cache，不调用运行期 `kd_mpi_venc_set_2d_osd_param()`。
3. 默认启动指纹为 `[diag] ai_branch=1`、`[diag] experiment=ai_osd_restore venc_osd=1 runtime_buffer_writes=1`；运动事件应同时带 `osd_enabled=1`。AI-off/no-OSD 诊断指纹仍由显式宏覆盖保留。
4. 主机侧源码规则、运动消抖、OSD buffer、DATAFIFO 和小核构建规则已执行；当前环境未连接 K230，阶段一的 10 分钟与阶段二的 20 次事件/10 分钟板端验收仍待现场完成。
5. 快照继续保留现有 DATAFIFO 行为；若板端仍记录 `params=0/0/0`，单独记为快照参数集问题，不回退 AI/OSD 视频链路。

### 2026-07-18：AI+OSD 默认版再次复现大面积运动停流

1. 用户提供的当前 ELF 启动指纹为 `ai_branch=1`、`experiment=ai_osd_restore`、`VENC 2D OSD init OK`、`AI motion thread create OK`，确认运行的是默认 AI+OSD 版本。
2. 日志在 seq 244 后出现 `[stall:venc] state=start cause=zero_pack`，`pending=0`、`cur_packs=0`，停流持续约 `12072ms` 后恢复；期间 AI 线程仍产生 event 6–10，故障首发不在 AI 线程退出、DATAFIFO pending 或 UDP 队列。
3. 小核日志只在大核停流恢复后出现数据，恢复阶段未先出现持续 `send queue busy`；当前证据继续指向主 VICAP→VENC 或 VENC 2D/OSD 内部状态，尚不能单独归因于 OSD buffer 更新。
4. 已补充 SCons 单变量覆盖：`scons VENC_OSD_ENABLE=0` 生成 AI-on/OSD-off 对照 ELF。临时构建已验证指纹为 `experiment=noosd_ab`、`ai_branch=1`、`AI motion thread create OK`，且不包含 `VENC 2D OSD init OK`。
5. 下一步必须在同一板端、使用每次启动新打印的 FIFO 地址，运行该 OSD-off ELF 进行至少10分钟和20次大面积运动对照；在获得结果前不修改默认 AI+OSD 配置。

### 2026-07-18：AI-off/OSD-on 启动即停产与 OSD VB pool 隔离

1. AI-off/OSD-on 对照启动指纹正确，大小核均使用 `phy_addr=0x1158c000`；大核仅编码1帧、投递到seq2后持续 `zero_pack`，`pending=0`，因此“无画面”首发仍在VENC而非FIFO地址或播放器。
2. AI-off 时原 `vb_init()` 只有主帧和VENC stream两个公共pool；`osd_init()` 使用 `VB_INVALID_POOLID` 申请192KiB块。SDK API明确警告用户态公共pool申请会减少内核可用块，官方VENC OSD示例也为OSD配置独立pool。
3. 本轮为OSD增加独立公共VB pool：OSD关闭时pool数量不变；OSD开启时增加一个 `OSD_BUF_CNT=1`、`OSD_BUF_SIZE=192KiB` 的精确大小pool。AI-off/OSD-on共3个pool，AI-on/OSD-on共4个pool，VENC的3块stream buffer保持不变。
4. `osd_init()` 新增 `OSD VB block acquired: pool_id/expected_pool_index/size` 指纹，用于板端确认OSD块进入独立pool。已重新生成 `big_app_aioff_osdon.elf`，板端稳定性待验证。

对照 ELF 构建使用 SCons 参数覆盖，不能只传 `CCFLAGS`（SDK 构建器不会将其稳定传播到应用对象）：

```bash
cd /home/ubuntu/workspace/big_core
RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin" \
  scons -c && \
RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin" \
  scons -j4 VENC_OSD_ENABLE=0
```

该命令生成 AI-on/OSD-off 对照 ELF，启动必须出现 `experiment=noosd_ab`、`ai_branch=1` 和 `AI motion thread create OK`，不得出现 `VENC 2D OSD init OK`。板端验证结束后再用默认命令重建 AI+OSD ELF。

## 2. 调试时间线

### 2026-07-17：快照标记和随机接入实验

1. 快照标记链路完成定位：运动事件先提交 OSD 显示和快照请求，后续导出帧携带 `reserved=0x1`。板端日志已经出现 `Snapshot request queued`、`[big] snapshot flag set` 和 `Snapshot request delivered to DATAFIFO`，说明大核标记链路已贯通。
2. 提交 `9cd4ee2` 增加快照相关 IDR 请求，`7003f69` 的记录表明 OSD 标记正常而 IDR 没有按预期触发。
3. 提交 `84e662b` 尝试启用 VENC 主动 IDR，随后 `29a3d53` 撤销 `kd_mpi_venc_enable_idr()` 和快照路径 `kd_mpi_venc_request_idr()`。可验证结论是该实验未形成可靠基线；当前方案明确保持主动 IDR 关闭，依赖 `GOP=8` 的自然随机访问帧。
4. 说明：现有证据只能证明“主动 IDR 未形成可靠可用方案并已撤销”，不能据此断言主动 IDR 是后续 VENC 停产的根因。

### 2026-07-17：15fps、GOP 和缓冲调整

1. 提交 `175fe37` 记录启动延迟约 240ms、随后快速增大。
2. 后续将 VICAP/VENC 目标输出保持 15fps、自然 GOP 调整为 8；大核 VENC output buffer 为 3，DATAFIFO pending 上限为 2、ring entries 为 3。
3. 小核 UDP socket 改为 `O_NONBLOCK`，请求 `SO_SNDBUF=128KiB`；Linux 实测 `sndbuf_actual=262144`，符合内核对请求值加倍的行为。
4. 非阻塞发送避免 `sendto()` 曾出现的约 1.55s 阻塞继续卡住 DATAFIFO reader；`EAGAIN/EWOULDBLOCK/ENOBUFS` 改为立即丢当前 RTP 包并等待自然随机访问帧。

### 2026-07-17 至 2026-07-18：RTP PTS 驱动与回退修复

1. RTP timestamp 改为由 VENC PTS 驱动，保留单调 RTP 序号、timestamp 和 SSRC。
2. 首帧 PTS 为 0 时先走固定 `RTP_TS_STEP=6000`；首次有效 PTS 从上一 RTP timestamp 之后建立基线。
3. PTS 回退或单次前跳超过 1s 时，异常帧只推进一个固定 step，并把当前 PTS 设为新基线。主机测试已证明下一帧恢复 `pts_mode=1`，避免永久退回固定步长。
4. 新增 `[rtp:rebase] count/from_pts/to_pts/rtp_ts`、`[rtp:diag] outq/clock_drift`。固定的 `clock_drift_ms` 偏移只表示两条时间基准存在常量差；只有该值持续单向增长才表示 RTP 时钟继续落后墙钟。

### 2026-07-18：DATAFIFO release、pending 与零 pack 自锁

1. 增加 `[ipc:submit-fail]`、`[ipc:drain]`、`NALU IPC READ_DONE`、`[datafifo:health]` 和大帧四阶段日志，分别观察提交阶段、release callback、读端确认和大帧处理。
2. `DATAFIFO_CMD_READ_DONE` 只证明小核已结束对当前描述符的使用；它不证明 VICAP/VENC 仍在继续生产新帧。
3. 已确认旧取流循环存在“`kd_mpi_venc_get_stream()` 返回成功但 `pack_cnt=0` 仍进入导出/释放”的风险。当前大核在导出前拦截零 pack，并在每轮 VENC query 前独立调用 `stream_export_flush()`，避免最后一个 pending 因没有下一帧而无法触发 release callback。
4. 日志中的长停顿期间，最后一个 `READ_DONE` 后 `pending=0`，但 `VENC empty stream` 仍从 count 25 持续增长，因此该次停顿不是“pending 一直占满”造成的自锁。

### 2026-07-18：OSD 串行化实验与长停顿证据

1. 提交 `7132cfe` 将 OSD 目标状态由 AI 线程提交、由 stream 线程在 VENC query/get 前串行调用 `kd_mpi_venc_set_2d_osd_param()`；状态交换只用短临界区，SDK 调用在临界区外，失败按 100ms 重试。
2. 该版本仍捕获两次明确停产：
   - seq 3489 后连续 `VENC empty stream`，恢复帧 seq 3490 的 `pts_delta_us=30079650`、`submit_delta_ms=30080`，约 30.08s。
   - seq 3532 后再次连续零 pack，恢复帧 seq 3533 的 `pts_delta_us=17074037`、`submit_delta_ms=17074`，约 17.07s。
3. 第一次长停顿恢复后，小核 UDP 队列从 0 快速升至 `outq_after=262400`，而 `sndbuf_actual=262144`；随后出现 `errno=11` 的 `send queue busy`，即 `EAGAIN`。小核进入 `wait_random=1`，跳过 17 个普通 VCL 后在自然 `IDR_W_RADL` 到来时发送缓存 VPS/SPS/PPS 并恢复。
4. 以上证据确认了“VENC 长时间无新包 -> 恢复时 UDP 队列打满 -> 丢包并等待自然随机访问”的故障链。它不单独证明运行期 OSD 参数更新就是 VENC 停产根因；二者当前仅存在时间相关性，需要用单变量实验验证。

### 2026-07-18：当前待上板版本

1. 大核运行期不再调用 `kd_mpi_venc_set_2d_osd_param()`。`osd_init()` 以全局 alpha 255 固定配置 region 0，初始素材 buffer 清零；显示时复制 512×96 ARGB 素材，隐藏时清零，再 flush MMZ cache。
2. OSD 请求仍保留 visible、deadline、generation 和 pending；cache flush 失败保留请求并按 100ms 重试。新日志指纹为 `[osd:buffer] generation/visible/cost_ms/ret/pending_after`。
3. 小核检测到 `rtp_clock.rebase_count` 增加时，在处理当前帧前进入 `wait_for_idr`、清零随机访问跳过计数并请求缓存参数集。当前帧若含自然 IDR/CRA 可立即恢复，否则跳过普通 VCL 等待下一随机访问帧。
4. 以上三项已通过源码规则测试和主机侧 RTP clock 测试，尚未完成板端 5 分钟压力验收，因此状态为“代码完成、板端待验证”，不能写成问题已解决。

### 2026-07-18：用户十分钟运行反馈与compact日志版

1. 用户确认实际连续运行约10分钟；在没有大面积画面变化时，播放延迟稳定在约330ms。为避免终端内容过长，`终端日志.md`只保存了临近结束的片段，不能据此还原完整10分钟内每个事件的精确时间线。
2. 同一轮反馈确认一旦出现大面积画面变化仍会卡死。现象说明“运行时长本身”不是充分触发条件，大面积变化与停流存在稳定关联；它仍不能单独证明根因位于VENC、OSD、码率突发或其他具体模块。
3. 结尾片段显示大核最后有效seq为2607，之后出现`VENC empty stream`且小核`avail_read=0`、`last_read_seq=last_read_done_seq=2607`，随后idle从504ms增长到4504ms。该片段再次把停顿边界定位在“小核已READ_DONE、上游没有新DATAFIFO消息”；由于用户在约4.5秒后手动退出，片段没有包含本次自然恢复。
4. 同一片段的快照请求seq2577、2595均返回`cannot build stream ... params=0/0/0`和`ret=-1`。因此当前板上版本的快照标志能到达小核，但小核没有缓存到构造可播放快照所需的VPS/SPS/PPS，这两次快照没有成功入队保存。该问题本轮只保留可诊断字段，不修改快照协议或保存算法。
5. 用户明确表示无法人工从十分钟日志中筛选若干字段。新compact日志版因此改为：正常期每60秒输出`[health:venc]/[health:ipc]/[health:small]`；连续500ms无有效stream/DATAFIFO数据后输出stall start，异常期每1秒ongoing，恢复时一条recovered。旧每秒、每15/30/150帧和正常大帧成功日志已移除。
6. 小核同步快照失败合并为首个及每10次一条`[snapshot:fail]`，并保留`stage/seq/flags/writer/frame_irap/cached_irap/cached_gop/params`，这样无需用户筛选即可直接确认是否仍为`params=0/0/0`。
7. compact版不改变编码、OSD、DATAFIFO协议、RTP重锁/自然IDR恢复或快照行为。正常10分钟大小核完整日志目标为不超过150行；该行数和大面积变化后的stall顺序仍需新ELF上板验证。

### 2026-07-18：完全禁用 VENC OSD 的单变量 A/B 实验版

1. 最新完整日志把长停顿首发点进一步缩到大核 VENC：seq 553 后先出现 `[stall:venc] state=start cause=zero_pack ... pending=0 cur_packs=0 pic_cnt=0`，之后至少持续到 `elapsed_ms=23585`；同期 AI 线程仍产生 event 11–17，说明运动检测旁路没有停。小核此前 `READ_DONE` 已追平且大核 `pending=0`，因此 UDP、DATAFIFO 和播放器不是该次停产的首发点。
2. 该日志对应的旧版本启动时明确出现 `nonai_2d_attach`、`VENC 2D OSD init OK` 和运行期 `[osd:buffer]`。只移除运行期 2D 参数更新仍会停产，所以本轮把唯一变量扩大为“整个 VENC 2D/OSD 子系统是否存在”。
3. 新大核默认 `VENC_OSD_ENABLE=0`：`osd_init()` 不申请 VB/MMZ、不 attach 2D、不下发 2D 参数；显隐、轮询和反初始化均为无副作用桩。启动唯一指纹为 `[diag] experiment=noosd_ab venc_osd=0 runtime_buffer_writes=0`，运动事件新增 `osd_enabled=0`。
4. 保持不变：1920×1080、4000kbps、30→15fps、GOP=8、运动检测、快照请求、DATAFIFO、RTP PTS 重锁、自然随机访问恢复和停流 compact 诊断。小核继续使用现有 `rtsp_sender_withsd_compactdiag`，不重编协议或算法。
5. 上板运行 10 分钟并制造至少 20 次大面积画面变化，只保存完整大小核日志。若不再出现 `[stall:venc]`，则 VENC 2D/OSD 链路是停产触发条件；若仍出现 `pending=0、cur_packs=0、pic_cnt=0`，则排除整个 OSD/2D 子系统，下一版采集 `/proc/umap/vicap` 和 `/proc/umap/venc` 区分主 VICAP 输出与 VENC 输入队列。
6. 本实验版不会显示 motion detected 画面标记；快照 `params=0/0/0` 仍是独立问题，本轮不修改。

### 2026-07-18：AI-off 稳定基线与1500kbps码率实验

1. 正确使用同一轮大核打印的 `phy_addr=0x1158c000` 后，大小核 DATAFIFO 恢复正常；此前小核打开旧地址 `0x11833000` 导致 `last_read_seq=0`，不是 AI-off 造成无视频。大核重启后必须重新复制地址，不能沿用旧值。
2. AI-off 正确地址版本运行约105秒，共编码1499帧，平均14.2fps；运行期 `pending=1`、`source_drift_ms=-4~-3ms`、`max_query_packs=1`、`pack_overflow_count=0`，未出现活跃期 VENC 停产。用户实测延迟约330ms，大面积画面变化抗干扰能力明显提高；该证据支持 AI/VICAP 多通道资源竞争会放大停产风险，但尚未完成600秒验收。
3. 剩余一次卡顿发生在小核发送 `seq=1162` 的472569字节关键帧时：`outq_after=262656` 已达到 `sndbuf_actual=262144`，随后 `send queue busy errno=11`，当前H.265 FU链不完整并等待7个VCL后从下一自然IDR恢复。
4. 该次异常前后 `clock_drift_ms` 分别约56ms和44ms，PTS增量仍约66696us，没有 `[rtp:rebase]`，因此不能归因于RTP前向PTS重锁；播放器延迟增加更符合UDP丢失关键帧后的重缓冲。
5. 用户选择低延迟优先并接受运动画质下降。本轮唯一运行参数改为 `ENC_BITRATE=1500kbps`，保持1080p、15fps、GOP=8、UDP请求缓冲128KiB、零packet pacing、非阻塞立即丢帧、DATAFIFO和RTP时钟不变；目标是从源头缩小关键帧突发。

### 2026-07-18：小核正常链路延迟画像

1. 在小核 RTP 发送线程加入独立 `latency_profile` 统计模块，覆盖 `msg_age`、copy、`READ_DONE`、RTP send 和 UDP outq 前后值；每类输出平均值、P90 和最大值。
2. 画像只在已有有效 outq 探针时采样，按5秒低频输出 `[latency:profile]` 后清空窗口，不改变 DATAFIFO、RTP、丢帧或自然随机访问恢复逻辑。
3. 主机测试、交叉编译和 `make verify` 已通过；尚未上板，因此该模块当前只提供延迟分段证据，不能据此宣称端到端延迟已下降。

## 3. 证据与假设表

| 状态 | 命题 | 关键证据 | 当前判断 |
| --- | --- | --- | --- |
| 已确认 | 快照标志从大核进入 DATAFIFO | `Snapshot request queued`、`[big] snapshot flag set ... reserved=0x1`、`Snapshot request delivered` | 大核快照标记链路正常；小核保存成功率是另一问题 |
| 已确认 | VENC 曾停止产出约 30.08s | 连续 `VENC empty stream`；恢复帧 `pts_delta_us=30079650`、`submit_delta_ms=30080` | 停顿位于新 VENC stream 产生之前 |
| 已确认 | VENC 曾再次停止产出约 17.07s | 连续零 pack；恢复帧 `pts_delta_us=17074037`、`submit_delta_ms=17074` | 问题可重复，不是单次退出噪声 |
| 已确认 | 长停顿期间 DATAFIFO pending 已释放 | 停顿开始处最后一个 `READ_DONE ... pending=0`，之后零 pack 仍增长 | “pending 满导致该次 VENC 停产”已排除 |
| 已确认 | 停顿恢复后 UDP 队列达到上限并 EAGAIN | `outq_after=262400`、`sndbuf_actual=262144`、`errno=11` | 网络发送路径在突发恢复时发生丢包 |
| 已确认 | 旧恢复依赖自然 IDR | `waiting random access` 后跳过 17 个 VCL，再出现 `sent cached VPS/SPS/PPS` 和 `random access start ... IDR_W_RADL` | 丢包后参考链恢复正确，但等待期间会卡顿 |
| 已确认 | 前向PTS大跳变按完整差值重锁 | `tests/test_rtp_clock.c` 对前向重锁当前帧和下一帧均断言 `pts_mode=1`，回退帧仍为单步 | 前向停顿时间不会永久累积为RTP时钟落后 |
| 已确认 | 无大面积画面变化时可连续运行约10分钟且延迟约330ms | 用户本轮完整运行反馈；终端只截取结尾片段 | 运行时长本身不是充分触发条件 |
| 已确认 | 本轮大面积画面变化后再次停流 | 用户现场反馈；结尾处seq停在2607，小核idle增长到4504ms且avail_read=0 | 故障仍可由大面积变化复现，具体根因未定 |
| 已确认 | compact日志中长停顿首发于大核VENC | seq553后`[stall:venc]`持续至少23585ms，`pending=0、cur_packs=0、pic_cnt=0`；AI事件仍增长 | 故障边界在主VICAP→VENC或VENC 2D内部，不在UDP/DATAFIFO/播放器 |
| 已确认 | AI-off 正确地址版本约105秒内主链路稳定 | 1499帧、平均14.2fps、`pending=1`、`source_drift_ms≈-4ms`，用户实测330ms | AI/VICAP多通道资源竞争会放大问题；仍需600秒验收 |
| 已确认 | 当前偶发卡顿由超大关键帧填满UDP队列 | seq1162总长472569字节，`outq_after=262656`、`errno=11`，随后等待7个VCL | 本次不是VENC停产或PTS重锁，优先削小编码突发 |
| 已确认 | 结尾两次快照未成功保存 | seq2577/2595均为`params=0/0/0`、`cannot build stream`、`ret=-1` | 标志已到小核，但缺少可播放快照所需参数集 |
| 已排除 | `READ_DONE` 完成等于上游 VENC 正常 | 小核 health/read_done 可正常，VENC 同时持续零 pack | 两者只能分别诊断，不能互相替代 |
| 已排除 | 客户端断开必然是首发根因 | 已观察到 VENC 先停产、UDP 后满、播放器随后卡顿/断开 | 客户端断开通常是停流结果；仍需按具体日志顺序判断 |
| 已排除 | 固定 `clock_drift_ms` 偏移等于延迟持续增长 | 队列恢复后 drift 可保持近似常量 | 应看 drift 的斜率和 outq，而非单个绝对值 |
| 已排除 | 只移除运行期 VENC 2D 参数更新即可消除零包 | 固定region、只写OSD buffer的版本仍在seq553后持续零pack | A/B变量升级为完全不attach VENC 2D |
| 仍待验证 | 整个 VENC 2D/OSD 子系统触发或放大 VENC 零包 | 旧版停顿时2D已attach且曾更新OSD buffer，尚无完全禁用版本板端结果 | 使用`noosd_ab`连续10分钟并制造至少20次大面积变化 |
| 仍待验证 | PTS 重锁后的新门控可在 1s 内恢复低延迟 | 源码顺序和自然 IDR 逻辑已测试，尚无板端重锁日志 | 需观察规定日志顺序和恢复后 outq/播放延迟 |
| 仍待验证 | 1500kbps可避免关键帧打满UDP发送队列 | 当前4000kbps曾产生472569字节关键帧；新参数仅通过主机规则测试 | 需在大面积运动下观察`max_frame_bytes/outq/EAGAIN`和播放延迟 |
| 仍待验证 | 若移除运行期 2D 参数后仍零包，问题在 VICAP/VENC 内部状态 | 当前没有停顿时 VICAP/VENC 状态探针 | 下一步才增加状态探针，不提前修改恢复策略 |

## 4. 当前稳定基线与构建

### 代码与参数基线

- 大核分支：`关闭ai线程，单变量验证画面问题`；主动 IDR 宏为0，自然 `GOP=8`，默认码率 `1500kbps`。
- 大核 AI：默认 `AI_BRANCH_ENABLE=1`，保留 `-DAI_BRANCH_ENABLE=0` 对照构建。
- 大核 OSD：默认 `VENC_OSD_ENABLE=1`，保留 `-DVENC_OSD_ENABLE=0` no-OSD 对照构建。
- 大核低延迟队列：`OUTPUT_BUF_CNT=3`、`NALU_IPC_PENDING_MAX=2`、`NALU_IPC_FIFO_ENTRIES=3`。
- 小核唯一维护目录：`/home/ubuntu/workspace/small_core`。
- 小核纳入 Git 的产物：`small_core/user/rtsp_sender_withsd_compactdiag`。
- 小核 UDP：非阻塞、请求 `SO_SNDBUF=128KiB`、无 RTP packet pacing。

### 大核构建与测试

```bash
cd /home/ubuntu/workspace
for t in big_core/mpp_pipeline/tests/*.sh; do sh "$t" .; done

cd /home/ubuntu/workspace/big_core
RTT_EXEC_PATH="/home/ubuntu/k230_sdk/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin" scons -j4
```

SCons 目标直接更新 `/home/ubuntu/workspace/big_core/big_app.elf`，不另建备份。

### 小核构建与测试

```bash
cd /home/ubuntu/workspace/small_core
make test
make build
make verify
```

### 板端与播放器

```bash
./rtsp_sender_withsd_compactdiag --fifo <NALU_PHY> --ctrl-fifo <CTRL_PHY>
```

`--ctrl-fifo` 只用于协议兼容，当前不启用主动 IDR。

```bash
ffplay -rtsp_transport udp -max_delay 0 -reorder_queue_size 0 \
  -fflags nobuffer -flags low_delay -framedrop -sync ext \
  -probesize 32 -analyzeduration 0 rtsp://<板端IP>/stream
```

### 关键日志指纹

- 初始化：`VB init OK`、`VENC chn=0 create OK`、`VICAP init OK`、`VI->VENC bind OK`。
- 默认恢复版：启动必须出现 `[diag] ai_branch=1`、`experiment=ai_osd_restore`，并看到 `VENC 2D OSD init OK`；运动事件应出现 `osd_enabled=1`。
- AI-off/no-OSD 对照：仅在显式覆盖宏时检查 `[diag] ai_branch=0` 或 `experiment=noosd_ab venc_osd=0 runtime_buffer_writes=0`，并确认对应分支无旁路/OSD副作用。
- 码率：启动必须出现 `bitrate_kbps=1500`；`[health:venc]` 实测目标为约1200–1800kbps。
- compact版本：大小核均出现`[diag] compact_diag normal=60s stall=500ms anomaly=1s`；正常期每60秒出现`[health:venc]/[health:ipc]/[health:small]`。
- OSD恢复版：启动必须出现`[diag] experiment=ai_osd_restore venc_osd=1 runtime_buffer_writes=1`和`VENC 2D OSD init OK`；运动事件、自动隐藏和重复触发需观察`[osd:buffer]`。
- 跨核：健康摘要中pending通常为0或短时1，`last_read_seq`应追上`last_read_done_seq`。
- 重锁恢复：`[rtp:rebase]` -> `PTS rebase, waiting random access` -> `sent cached VPS/SPS/PPS` -> `random access start`。
- 网络：`outq_before/outq_after` 不应持续贴近 `sndbuf_actual`，不应重复 `send queue busy`。
- 停顿：以`[stall:venc]`和`[stall:small]`的start/ongoing/recovered直接关联；不再查找旧`VENC empty stream`或idle轮询行。
- 快照：失败时`[snapshot:fail]`必须保留stage及params；当前若仍为`params=0/0/0`，表示本轮日志精简生效但快照缺参数集问题尚未修复。
- 退出：手动停止后必须出现 `Pipeline test PASSED.`。

## 5. 本轮上板验收清单

1. 启动大核后，从同一轮日志复制最新 `NALU IPC DATAFIFO phy_addr` 给小核；复制后不得再次重启大核。
2. 启动日志必须出现 `ai_branch=0`、`bitrate_kbps=1500` 和无OSD指纹；运行期不得出现AI线程、运动事件或VENC 2D日志。
3. 连续运行至少600秒并反复制造大面积画面变化；保留大小核完整日志。
4. `[health:venc]` 实测码率目标为1200–1800kbps，目标 `max_frame_bytes < 196608`；pending稳定在0–1。
5. 活跃运行期不得出现超过500ms的 `[stall:venc]`；手动退出后的停流日志不计失败。
6. 不得出现 `[rtp] send queue busy` 或 `[datafifo:large] stage=rtp ret=-1`；UDP outq不得持续达到实际 `SO_SNDBUF`。
7. `clock_drift_ms` 保持在±100ms；若发生PTS重锁，核对参数集和自然随机访问恢复顺序。
8. VLC延迟以330ms为基线；运动或短暂异常后应在2秒内回到 `330ms±100ms`，不得永久增加。
9. 若1500kbps仍出现UDP队列满，本轮不扩大缓冲、不增加阻塞重试；下一轮单独降到1000kbps。
10. 手动退出并确认 `Pipeline test PASSED.`。

## 6. 经验与教训

- 每轮先核对实际部署 ELF 的文件名、修改时间、字符串指纹和启动日志；源码正确不能证明板上运行的是同一版本。
- 客户端断开通常是上游停流、网络丢包或解码等待的结果，必须按时间顺序找第一个异常。
- 固定时钟偏移不等于延迟持续增长；同时观察 RTP/墙钟差值的斜率、UDP outq 和实际播放延迟。
- `READ_DONE` 完成只表示当前 DATAFIFO item 生命周期结束，不证明 VICAP/VENC 正在继续生产。
- SDK 返回成功后仍要验证输出结构；`get_stream ret=0` 但 `pack_cnt=0` 必须作为空输出处理。
- 根因修复与故障恢复应分开设计：移除运行期 2D 参数更新用于验证根因，自然 IDR 门控用于在停顿已发生时恢复干净参考链。
- 每轮尽量只改变一个关键变量；若同时改分辨率、码率、缓冲和 GOP，就无法从结果归因。
- 大段原始终端输出保留在 `终端日志.md`；本日记只保存能支持判断的字段和结论。

## 7. 后续实验模板

### 实验：<简短名称>

- 日期：
- 大核 ELF 路径、修改时间、版本指纹：
- 小核 ELF 路径、修改时间、版本指纹：
- 唯一关键变量：
- 保持不变的参数：
- 复现步骤：
- 预期结果：
- 实际结果：
- 证据：日志字段、时间点、截图或抓包文件；不要粘贴无关长日志。
- 结论：已确认 / 已排除 / 仍待验证。
- 对原结论的修正：没有则写“无”；有则引用原实验并说明新证据。
- 下一步：
