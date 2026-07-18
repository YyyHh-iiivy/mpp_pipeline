首先需要连接上wifi：
1. ifconfig wlan0 up//使能网卡
2. wpa_supplicant -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf
-B//连接wifi，需要先修改/etc/wpa_supplicant.conf文件，填入wifi的ssid和密码
3. udhcpc -i wlan0//获取 IPv4 地址

## 编译

在 `small_core/` 目录执行：

```bash
make test
make
make verify
```

默认使用 `/home/ubuntu/k230_sdk` 内的 Xuantie glibc 工具链和 Linux 小核
`slave/lib/libdatafifo.a`，生成：

```text
user/rtsp_sender_withsd_compactdiag
```

仓库跟踪 `small_core/user/` 中需要发布的可执行文件，便于从 GitHub 直接下载；
本轮纳入 `rtsp_sender_withsd_compactdiag`。历史 ELF 与 `girlshy.h265` 未迁入，
Makefile 中的历史目标名仅用于说明诊断演进。该目标包含精简正常日志、异常诊断
和 PTS 重锁后的自然随机访问等待。

## 运行

确认网络没问题后，即可打开小核应用：

```bash
./rtsp_sender_withsd_compactdiag --fifo 0x12ffa000
```

如果大核日志同时打印了 CTRL IPC DATAFIFO 地址，推荐使用：

```bash
./rtsp_sender_withsd_compactdiag --fifo <NALU_PHY> --ctrl-fifo <CTRL_PHY>
```

启动时必须看到以下版本指纹，才能确认运行的是新版非阻塞程序：

```text
[rtp] UDP socket low-latency mode: nonblock=1 sndbuf=131072
[rtp] UDP socket buffers: sndbuf_request=131072 sndbuf_actual=...
[diag] compact_diag normal=60s stall=500ms anomaly=1s
```

正常运行不再按帧输出RTP诊断。只有RTP时间步异常、`SIOCOUTQ`查询失败，
或UDP发送队列达到实际 `SO_SNDBUF` 的75%时才输出：

```text
[rtp:diag] frame_seq=... outq_before=... outq_after=... outq_high=... sndbuf_actual=... rtp_step=... rtp_elapsed_ms=... wall_elapsed_ms=... clock_drift_ms=... query_fail=...
```

- `outq_high` 随画面延迟同步增长：优先检查板端 UDP/Wi-Fi 排队。
- `outq` 接近 0 且 `clock_drift_ms` 稳定：优先检查网络下游或播放器队列。
- `clock_drift_ms` 持续增长：检查 RTP 时间轴生成。
- 出现PTS回退或超过1秒的前向跳变时会输出 `[rtp:rebase] count=... from_pts=... to_pts=... rtp_ts=...`。前向大跳变按完整PTS差换算90kHz RTP delta，当前帧保持`pts_mode=1`并重建基线；PTS回退仍只推进一个step，下一帧恢复`pts_mode=1`。
- PTS重锁后紧接着输出 `PTS rebase, waiting random access`，不重置RTP序号、SSRC或会话；普通VCL暂停发送，下一自然IDR/CRA到来时先发缓存VPS/SPS/PPS再恢复。
- 修复版前两帧的 `rtp_step` 都应为正值，seq=2 不应再出现 `-6000`。

compact诊断版正常时每60秒输出一次大小核关联所需的摘要：

```text
[health:small] uptime_s=... last_read_seq=... last_read_done_seq=... last_sent_seq=... avail_read=... idle_ms=... play=... wait_random=... frames=... rtp_drop=... outq=... sndbuf=...
```

连续500ms没有收到有效DATAFIFO消息时输出一次 `state=start`，之后每秒输出
一次 `state=ongoing`，数据恢复时输出一次 `state=recovered`：

```text
[stall:small] state=... cause=... elapsed_ms=... last_read_seq=... last_read_done_seq=... avail_read=... play=... wait_random=...
```

超过64KiB的大帧正常完成时保持静默；复制或READ_DONE超过5ms、RTP发送超过
15ms，或任一阶段失败时，才输出对应 `[datafifo:large]`。断流时：

- `last_read_seq > last_read_done_seq`：小核卡在复制、映射或READ_DONE。
- 两者相等但大核仍未释放：大核DATAFIFO callback drain链路异常。

`--ctrl-fifo` 仅保留协议兼容；当前大核配置不启用主动 IDR。

然后可用VLC打开网络串流，地址为板子地址
例如： rtsp://10.239.114.28

然后即可启动大核程序

如果想要先验证RTSP流是否正常
可用文件模式打开RTSP流，例如在小核执行：
./rtsp_sender_withsd_compactdiag girlshy.h265
girlshy.h265为一个测试视频文件，可用VLC打开验证
最好和程序放在同一个目录下

保存快照需要先确保已挂载sd卡

当前构建的 `SNAPSHOT_ENABLE_FFMPEG_CONVERT=0`，成功快照保存为
`/sdcard/snapshots/snapshot_XXXXXX.h265`，不会生成JPG；本轮只精简日志，
没有改变快照协议和保存行为。

检查挂载：mount | grep sdcard

换用ffplay的低延迟模式后，延迟降低了
调用指令如下：
ffplay -rtsp_transport udp -max_delay 0 -reorder_queue_size 0 -fflags nobuffer -flags low_delay -framedrop -sync ext -probesize 32 -analyzeduration 0 rtsp://板端ip/stream
