/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_SX1262_H
#define _UAPI_LINUX_SX1262_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SX1262_ABI_VERSION 2
#define SX1262_MAX_PAYLOAD 255
#define SX1262_MAX_BATCH 8
#define SX1262_MAX_RADIOS 8

/* All reserved fields must be zero. Native endian, identical ILP32/LP64 layout. */
struct sx1262_config {
	__u32 version;
	__u32 frequency_hz;
	__u32 bandwidth_hz;
	__s32 tx_power_dbm; /* module output power, not necessarily chip power */
	__u32 tx_preamble_symbols;
	__u32 rx_preamble_symbols;
	__u32 silence_timeout_ms; /* 0 disables silence recovery; default 60000 */
	__u16 sync_word;
	__u8 spreading_factor; /* 5..12 */
	__u8 coding_rate; /* denominator: 5..8 */
	__u8 crc;
	__u8 invert_iq;
	__u8 implicit_header;
	__u8 implicit_length; /* 1..255 when implicit_header == 1 */
	__u8 ldro; /* 0 auto, 1 off, 2 on */
	__u8 hard_reset_after; /* 1..100 consecutive watchdog recoveries */
	__u8 reserved[6];
};

struct sx1262_frame {
	__u16 length;
	__u8 data[SX1262_MAX_PAYLOAD];
	__u8 reserved;
};

/* An accepted batch owns the global TX queue until all its frames finish. */
struct sx1262_tx_batch {
	__u32 version;
	__u32 count;
	__aligned_u64 cookie;
	struct sx1262_frame frames[SX1262_MAX_BATCH];
};

#define SX1262_EVENT_RX 1
#define SX1262_EVENT_TX 2

/* read() returns exactly one complete record. TX records are never dropped. */
struct sx1262_event {
	__u32 version;
	__u32 type;
	__aligned_u64 timestamp_ns; /* CLOCK_BOOTTIME at software observation */
	__aligned_u64 cookie; /* TX: batch cookie; write() uses zero */
	__s32 status; /* TX: 0 or negative errno */
	__u16 length; /* RX payload length */
	__u16 frames_sent; /* TX: frames with confirmed TX_DONE */
	__s16 rssi_half_dbm;
	__s16 snr_quarter_db;
	__u8 data[SX1262_MAX_PAYLOAD];
	__u8 reserved[5];
};

struct sx1262_stats {
	__u32 version;
	__u32 online;
	__aligned_u64 rx_packets;
	__aligned_u64 rx_dropped;
	__aligned_u64 rx_crc_errors;
	__aligned_u64 tx_packets;
	__aligned_u64 tx_errors;
	__aligned_u64 soft_recoveries;
	__aligned_u64 hard_recoveries;
	__aligned_u64 polled_irqs;
	__s32 last_error;
	__u32 tx_pending;
	__u32 rx_enabled;
	__u32 reserved;
};

#define SX1262_IOC_MAGIC 'L'
#define SX1262_IOC_GET_CONFIG _IOR(SX1262_IOC_MAGIC, 0, struct sx1262_config)
#define SX1262_IOC_SET_CONFIG _IOW(SX1262_IOC_MAGIC, 1, struct sx1262_config)
#define SX1262_IOC_TX_BATCH _IOW(SX1262_IOC_MAGIC, 2, struct sx1262_tx_batch)
#define SX1262_IOC_GET_STATS _IOR(SX1262_IOC_MAGIC, 3, struct sx1262_stats)
#define SX1262_IOC_SET_RX _IOW(SX1262_IOC_MAGIC, 4, __u32)
#define SX1262_IOC_RESET _IO(SX1262_IOC_MAGIC, 5)

#endif
