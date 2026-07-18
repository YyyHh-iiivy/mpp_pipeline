# V2 当前小核低延迟版

## 当前结论

`startidr-dup` 版本中，FFplay 出现：

```text
The cu_qp_delta 94 is outside the valid range [-26, 25].
Skipping invalid undecodable NALU: 19
```

这说明 IDR NALU 本身被解坏。包级 duplicate 使用相同 RTP sequence 重发 FU 片段，可能被 FFmpeg depacketizer 拼进同一个 NALU，导致 IDR 负载损坏。因此当前 V2 已撤销启动包级 duplicate。

## 当前保留的改动

- 只修改小核，大核保持 GitHub 原始参数。
- 保留 RTP socket 非阻塞，避免 `sendto()` 长时间阻塞 DATAFIFO reader。
- 保留发送失败后等待随机接入帧恢复。
- 保留 RTSP 新 PLAY 后重置 RTP seq/timestamp。
- 保留 PLAY 后随机接入门控：第一帧 VCL 必须是 BLA/IDR/CRA。
- 保留 VPS/SPS/PPS 贴近随机接入帧发送。
- 默认禁用板端 ffmpeg JPG 转换，只保存 `.h265` snapshot。
- 关闭 `RTP_STARTUP_DUPLICATE_PACKETS`。
- 调整 `RTP_PACKET_PACE_US=0`，优先降低大帧 RTP 分片发送耗时；如网络丢包明显再回调小间隔。
- SDP 增加 `a=framerate:15`，减少播放器把 15fps 流猜成 30fps 的概率。
- 小核新增可选 `--ctrl-fifo <phy>`，用于 PLAY 时向大核请求 IDR。
- 小核 DATAFIFO RTP timestamp 改为 VENC PTS 驱动，异常时回退固定步长。

## 启动方式

大核启动后会打印：

```text
NALU IPC DATAFIFO init OK: phy_addr=...
CTRL IPC DATAFIFO init OK: phy_addr=...
```

小核推荐启动：

```sh
./rtsp_sender_withsd --fifo <NALU_PHY> --ctrl-fifo <CTRL_PHY>
```

如果暂时不传 `--ctrl-fifo`，旧命令仍可运行，但 PLAY 时不会主动请求大核 IDR，只依赖 GOP fallback。

## 测试命令

```sh
ffplay -rtsp_transport udp -max_delay 0 -reorder_queue_size 0 -fflags nobuffer -flags low_delay -framedrop -sync ext -probesize 32 -analyzeduration 0 -strict experimental rtsp://10.94.172.28/stream
```

## 下一步判断

如果当前版仍出现 POC 错误，但小核日志显示：

```text
[rtp] random access start: skipped_vcl=0 type=19(IDR_W_RADL)
```

则说明小核已经从 IDR 开始发，问题更可能在 RTP/H.265 分片、UDP 丢包或播放器侧 depacketize。当前版已加入 200us RTP 包间隔；如果仍完全一样，下一步建议用 TCP RTSP 做对照，或抓包检查 IDR 的 FU 分片是否完整、有序。
