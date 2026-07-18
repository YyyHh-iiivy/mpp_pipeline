SDK_ROOT ?= /home/ubuntu/k230_sdk
TOOLCHAIN_DIR ?= $(SDK_ROOT)/toolchain/Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0/bin
CROSS_COMPILE ?= $(TOOLCHAIN_DIR)/riscv64-unknown-linux-gnu-

CC := $(CROSS_COMPILE)gcc
READELF := $(CROSS_COMPILE)readelf
STRINGS := $(CROSS_COMPILE)strings

TARGET := user/rtsp_sender_withsd_compactdiag
PRESERVED_REBASE_IDRWAIT_TARGET := user/rtsp_sender_withsd_rebase_idrwait
PRESERVED_STABILITYDIAG_TARGET := user/rtsp_sender_withsd_stabilitydiag
PRESERVED_QUEUEPROBE_TARGET := user/rtsp_sender_withsd_queueprobe
PRESERVED_RTPCLOCK_TARGET := user/rtsp_sender_withsd_rtpclockfix
PRESERVED_RTPREBASE_TARGET := user/rtsp_sender_withsd_rtprebase

SOURCES := \
	src/RTSP_Create.c \
	src/datafifo_snapshot.c \
	src/file.c \
	src/nalu_datafifo.c \
	src/rtp.c \
	src/rtp_clock.c \
	src/rtsp.c \
	src/small_diag.c \
	src/snapshot_writer.c

HEADERS := \
	lnc/datafifo_snapshot.h \
	lnc/file.h \
	lnc/nalu_datafifo.h \
	lnc/rtp.h \
	lnc/rtp_clock.h \
	lnc/rtsp.h \
	lnc/small_diag.h \
	lnc/snapshot_writer.h

DATAFIFO_ROOT := $(SDK_ROOT)/src/common/cdk/user/component/datafifo

CPPFLAGS := \
	-Ilnc \
	-I$(SDK_ROOT)/src/big/mpp/include \
	-I$(DATAFIFO_ROOT)/include
CFLAGS := -O2 -g
LDFLAGS := -L$(DATAFIFO_ROOT)/slave/lib
LDLIBS := -ldatafifo -lpthread

.PHONY: all build test verify clean

all: build

build:
	mkdir -p $(dir $(TARGET))
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $(TARGET)

test:
	sh tests/test_workspace_build_rules.sh
	sh tests/test_rtp_queue_probe.sh
	sh tests/test_rtp_clock.sh
	sh tests/test_datafifo_health_diag.sh
	sh tests/test_rebase_idr_wait.sh
	sh tests/test_compact_diag.sh

verify:
	test -x $(TARGET)
	file $(TARGET) | grep -q 'UCB RISC-V'
	$(READELF) -Ws $(TARGET) | grep -q 'fcntl@'
	$(READELF) -Ws $(TARGET) | grep -q 'ioctl@'
	$(STRINGS) $(TARGET) | grep -q 'UDP socket low-latency mode: nonblock=1'
	$(STRINGS) $(TARGET) | grep -q 'pts_delta_us'
	$(STRINGS) $(TARGET) | grep -q '\[rtp:diag\] frame_seq='
	$(STRINGS) $(TARGET) | grep -q 'PTS rebase, waiting random access'
	$(STRINGS) $(TARGET) | grep -q '\[diag\] compact_diag normal=60s stall=500ms anomaly=1s'
	$(STRINGS) $(TARGET) | grep -q '\[health:small\]'
	$(STRINGS) $(TARGET) | grep -q '\[stall:small\]'

clean:
	rm -f $(TARGET)
