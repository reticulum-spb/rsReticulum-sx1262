# rsReticulum SX1262 plugin

C11 plugin connecting `rsReticulum` to the **sx1262-linux** kernel driver through
`/dev/loraN`. RNode air framing is unchanged: one header byte, a four-bit sequence
and up to two LoRa frames for a Reticulum packet of at most 500 bytes.

SPI, GPIO, IRQ, TCXO, PA configuration and radio recovery belong to the kernel
driver and Device Tree. This plugin does not open spidev or GPIO devices.
Each instance exclusively opens one LoRa device and owns its event worker,
TX completion state and reassembly buffer.

## Build

Dependencies: Linux, CMake 3.16+, C11 compiler, pthreads, libm and libcyaml 1.4.2 or 2.x.
libgpiod is no longer required.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

The installed filename is `sx1262.so`, without a `lib` prefix, normally in
`/usr/lib/reticulum-rs`. The host ABI header `include/rns_plugin.h` is vendored
from rsReticulum. `include/sx1262_uapi.h` is an unmodified copy of ABI version 2
from sx1262-linux (`include/uapi/linux/sx1262.h`, Linux-syscall-note license).
Neither neighbouring checkout is required to build the plugin.

## Configuration and migration

See `example.yaml`. Required fields: `device`, `frequency`, `bandwidth`,
`spreading_factor`, `coding_rate`, `tx_power`. Unknown keys are rejected.
Replace the old `spi`, `cs`, `rst`, `busy`, `dio1`, `rx_en`, `tx_en` and
`tcxo_voltage` fields with `device: /dev/lora0`. Hardware settings now belong
in Device Tree. Numbering follows driver probe order; a stable device symlink
can be used. The process needs read/write permission (driver default: `0600`).
Two interfaces cannot open the same radio.

| Optional field | Default | Meaning |
| --- | ---: | --- |
| `rx_preamble_symbols` | 65 | RX acquisition window; minimum 12 at SF5/6, otherwise 4; maximum 65535 |
| `tx_preamble_symbols` | 25 | Minimum 12 at SF5/6, otherwise 4; maximum 65535 |
| `sync_word` | 5156 | `0x1424` |
| `irq_watchdog_seconds` | 60 | Kernel silence watchdog, 1–86400 seconds |
| `hard_reset_after` | 2 | Kernel recovery threshold, 1–100 |
| `tx_timeout_seconds` | 120 | Userspace wait for an accepted batch, 1–86400 seconds |

SF is 5–12; CR is the denominator 5–8 (CR=4 is no longer accepted).
Bandwidths: 7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000,
250000 and 500000 Hz. The parser permits TX power −9..30 dBm; the driver
validates it and frequency against the actual DT hardware profile. For E22 M30S
this is normally 8..30 dBm. PHY combinations exceeding the driver's airtime
limit are rejected by `SET_CONFIG`.

The plugin explicitly selects CRC on, normal IQ, explicit header and **LDRO off**
(`ldro=1` in the driver ABI), matching the previous plugin. It does not inherit
these settings from a previous process. Bitrate is reported to the host at startup.
The embedded JSON Schema describes the new configuration for the rsReticulum UI.

## TX, RX and recovery

A whole Reticulum packet is submitted using one `TX_BATCH` with a unique cookie.
`send()` succeeds only after a matching TX event reports status zero and all
frames sent. Queue acceptance alone is not success. Partial completion, a
failure to return to RX, or a submission error returns an error to the host.
There is no automatic retry and no per-batch cancellation.

There is at most one outstanding batch per instance. When the userspace wait
expires, `send()` returns an error but the accepted batch **may still transmit**.
New sends are rejected until its completion is consumed. The event worker marks
the interface offline while an expired batch remains outstanding, logs the late
result and restores availability when the driver is online. An unrelated cookie
cannot release the outstanding batch. Increasing `tx_timeout_seconds` may be
needed with slow PHY settings or several radios sharing the global TX queue.
The timeout includes queue wait; it is not a physical TX deadline.

RX and TX events are consumed by one worker. RX frames are reassembled into
Reticulum packets; RSSI is converted from half-dBm units with truncation toward
zero and SNR from quarter-dB units with nearest rounding, preserving the previous
plugin's conversions. `GET_STATS` tracks driver online/offline status at roughly
100 ms intervals. Recovery is performed by the driver, not userspace resets.
Device removal or an invalid event ABI stops the worker and leaves the interface
offline; recreating the interface is required after rebind.

Shutdown marks the interface offline, wakes and joins the event worker, then
closes the device. Driver close cancels any remaining session work. The host
joins its TX worker before destroying the plugin, so shutdown can first wait
up to the configured send timeout (plus kernel operation/scheduling delays).

## Access to the medium

The updated sx1262-linux driver waits for an already detected RX to finish,
then applies a random backoff of 0..336 ms in 24 ms slots and checks RX again.
The access deadline is twice the configured maximum-frame airtime plus 1000 ms.
On expiry it completes the batch with `-EBUSY` and zero frames sent; that batch
will not transmit later. IRQ handling and RX watchdog continue during this wait.
There is no added contention delay between frames in one batch.

This is not CAD or full carrier sensing: undetected signals and hidden nodes
can still collide. All driver radios share one ordered TX queue. ABI version 2
is unchanged: the plugin also works with the earlier driver, but that driver
does not provide the new RX-aware backoff policy.

## Tests

Host tests cover framing, YAML parsing, dynamic ABI loading, and a fake Linux
record device driving the actual event thread. Backend tests exercise batching,
RX metadata, partial/error completion, queue rejection, timeout, late/wrong-cookie
completion, online recovery and device removal. No radio is needed for these.

For a manual ABI integration test, build with `-DBUILD_HARDWARE_TEST=ON` and run:

```sh
./build/hardware_test /usr/lib/reticulum-rs/sx1262.so radio.yaml rx
./build/hardware_test /usr/lib/reticulum-rs/sx1262.so radio.yaml tx
```

`radio.yaml` contains only the plugin mapping (no `interfaces:` wrapper).
`rx` opens the radio for five seconds; `tx` transmits 1/254/255/500-byte packets.
The `late` mode needs `tx_timeout_seconds: 1` and PHY settings where a 500-byte
packet takes longer than one second; it checks rejection while pending and
restored availability after the late completion. TX modes emit real RF.
The hardware test is deliberately not registered with CTest.

On hardware check both 254-byte and 500-byte Reticulum packets in both directions
against an existing RNode, two radios contending for the global queue, RX during
backoff, a stuck RX/watchdog recovery, and a deliberately short userspace TX
wait followed by late completion. Confirm the newer driver rejects sustained
busy-medium attempts with zero frames sent. RF compatibility, timing and power
must be checked on the target board; host tests do not establish them.

## Origin and license

RNode air framing is derived from RNode-linux, copyright 2025 Belousov Oleg
(R1CBU). Plugin license: LGPL-2.1-or-later. The kernel UAPI header retains its
own SPDX identifier, GPL-2.0 WITH Linux-syscall-note.

Version 0.3 requires sx1262-linux ABI 2 (44-byte configuration). Upgrade the
module and plugin together. Replace the old `preamble_symbols` key with
`tx_preamble_symbols` and set `rx_preamble_symbols` to the longest peer preamble
(default 65). The old key is rejected to avoid silently losing its value.
RX preamble affects RX/header acquisition and medium-access waits; TX preamble
affects transmitted packets and TX airtime only. They need not be equal.
