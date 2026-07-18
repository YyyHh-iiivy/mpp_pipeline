# V2 小核稳定性修复说明

## 日志结论

你提供的日志中，关键异常是：

```text
[datafifo] frame seq=182 ... total=77509 ... send=1550ms play=1
```

RTP 发送线程在 `sendto()` 上阻塞了约 1.55 秒。DATAFIFO reader 线程被卡住后，虽然当前 item 已经 READ_DONE，但后续 item 无法继续读取；DATAFIFO entries 只有 3，大核侧很容易在这段时间内写满并出现停流/断连现象。

日志中的 snapshot 也有一个问题：板端没有 `ffmpeg`，worker 每次都尝试转换 JPG 并失败。它不是这次断流的第一嫌疑，但会增加无意义的 shell/IO 开销。

## 本次小核改动

- 保持 DATAFIFO `copy -> READ_DONE -> RTP send` 顺序，不恢复 V1 direct-send。
- RTP UDP socket 设置为非阻塞，并设置适中的 `SO_SNDBUF`。
- `sendto()` 遇到 `EAGAIN` / `EWOULDBLOCK` / `ENOBUFS` / `EINTR` 时立即返回 `RTP_SEND_DROP`，不阻塞直播线程。
- 上层收到 RTP drop 后丢弃当前帧，重新要求发送 VPS/SPS/PPS，并等待下一个 IDR 恢复。
- RTSP 每次新的 PLAY 自增 session generation，小核 RTP 线程重置 seq/timestamp 并等待 IDR。
- RTSP TEARDOWN 或 TCP 断开时清掉旧 client RTP 目标，避免重连状态残留。
- 默认禁用 snapshot 的 ffmpeg JPG 转换，只保存 `.h265` 快照流。

## 测试命令

板端：

```sh
./rtsp_sender_withsd --fifo 0x12510000
```

PC 端：

```sh
ffplay -rtsp_transport udp -max_delay 0 -reorder_queue_size 0 -fflags nobuffer -flags low_delay -framedrop -sync ext -probesize 32 -analyzeduration 0 -strict experimental rtsp://10.94.172.28/stream
```

## 观察点

- 正常情况下不应再出现 `send=1550ms` 这类长时间发送阻塞。
- 如果网络拥塞，应看到 `[rtp] send queue busy` 或 `[rtp] send ret=1 ... wait for IDR`，随后等下一个 IDR 恢复。
- 重复关闭/打开 ffplay 时，应看到 `[rtp] new session generation=...`。
- Snapshot 日志应变为保存 `.h265`，不再出现 `sh: ffmpeg: not found`。
