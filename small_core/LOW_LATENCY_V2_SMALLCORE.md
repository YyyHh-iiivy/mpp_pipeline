# V2 小核限定低延迟测试版

## 目标

本目录用于重新测试 V2 小核侧低延迟效果。工程来自 GitHub 当前分支 `k230-lightweight-video-streaming-server-main` 的干净模板，只在 `small_core` 下增加/更新说明文件，大核代码保持原样。

## 本版边界

- 未修改 `big_core`。
- 未引入 V1 的 DATAFIFO direct-send 路径。
- 未修改 RTP socket 为非阻塞模式。
- 未引入 V3 的 PTS 驱动 RTP timestamp。
- 保留 GitHub 当前小核代码中的既有低延迟策略：丢弃积压旧帧、降低热路径日志、直播发送优先于 snapshot。

## 大核基线

本版应保持以下 GitHub 原始配置：

```c
#define SRC_FPS      30
#define INPUT_BUF_CNT   6
#define OUTPUT_BUF_CNT  4
```

也就是说，这次测试不再包含此前 V2-analysis 里对大核帧率和 buffer 数量的调整。

## 小核测试命令

板端启动：

```sh
./rtsp_sender --fifo 0x12ffa000
```

PC 端使用你当前的 FFplay 命令：

```sh
ffplay -rtsp_transport udp -max_delay 0 -reorder_queue_size 0 -fflags nobuffer -flags low_delay -framedrop -sync ext -probesize 32 -analyzeduration 0 -strict experimental rtsp://10.94.172.28/stream
```

如果换板端 IP，只替换 URL 中的 IP，其他参数保持不变。

## 对比建议

1. 先只跑 RTSP，不触发 snapshot。
2. 等首个 IDR 后再观察稳态延迟，不把首次 PLAY 等 IDR 的时间算入。
3. 如果这一版比 V1 稳定或延迟更低，说明 V1 direct-send/socket 改造可能带来了副作用。
4. 如果这一版仍接近 0.5 到 0.6 秒，再继续用 V3 小核 PTS 打点版判断是发送节奏问题、编码输出节奏问题，还是播放器/解码显示队列问题。

## 快速确认

在工程根目录执行：

```sh
grep -R "datafifo_process_msg_direct_for_playback\\|O_NONBLOCK\\|SO_SNDBUF\\|rtp_clock_make_timestamp" small_core
```

正常情况下应无输出，表示未混入 V1/V3 代码路径。
