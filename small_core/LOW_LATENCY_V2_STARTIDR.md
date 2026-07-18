# V2 小核随机接入启动门控

## 现象解释

FFplay 启动时出现：

```text
Could not find ref with POC 0
...
Could not find ref with POC 13
```

这说明播放器收到了需要参考帧的 H.265 VCL NALU，但它还没有拿到可独立解码的随机接入帧。POC 数量和启动点距离下一个 IDR/CRA/BLA 的帧数高度相关。30fps 下 13 帧约 433ms，所以这会直接表现为 0.5 秒级启动延迟；偶然低延迟时 POC 不到 10，说明那次 PLAY 刚好离下一个随机接入帧更近。

## 小核侧改动

- PLAY 新会话后强制进入随机接入门控。
- 等待期间丢弃普通 VCL NALU，不向播放器发送依赖参考帧的 P 帧。
- 随机接入类型扩展为 BLA/IDR/CRA，而不是只认 IDR。
- 随机接入帧到来时，立即先发 cached VPS/SPS/PPS，再发该随机接入帧。
- 日志输出 `waiting random access, skip vcl count=...` 和 `random access start: skipped_vcl=...`，用于和 FFplay 的 POC 数量对照。

## 仍然无法由小核解决的部分

小核不能让大核 VENC 立刻产生 IDR。如果希望每次 PLAY 都稳定低延迟，需要大核在收到小核 PLAY/请求后调用 VENC 强制 IDR，或者缩短 GOP/IDR interval。本版只能保证小核不主动发送不可解码的 P 帧，并把参数集贴近随机接入帧发送。

## 观察点

正常启动时，小核应出现类似：

```text
[rtp] new session generation=2, reset RTP seq/timestamp and wait random access
[rtp] waiting random access, skip vcl count=1 type=1(NAL)
[rtp] random access start: skipped_vcl=13 type=19(IDR_W_RADL) len=...
```

如果 `skipped_vcl` 经常是 10 到 13，说明启动延迟主要来自 GOP 相位；如果它接近 0，但 FFplay 仍然报 POC，则继续检查 RTP 丢包或 H.265 NALU 分片完整性。
