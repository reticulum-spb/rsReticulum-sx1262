# rsReticulum SX1262 plugin

C11 plugin for direct use of an SPI SX1262 radio by `rsReticulum`. It uses the
stable ABI from `rsReticulum/crates/rns-plugin` and keeps the RNode over-air
format: one header byte, a four-bit packet sequence and two-frame fragmentation
for Reticulum packets up to 500 bytes.

`include/rns_plugin.h` is a byte-for-byte vendored copy of
`rsReticulum/crates/rns-plugin/include/rns_plugin.h`. It is intentionally kept
inside this repository so the plugin can be built without a neighbouring
rsReticulum source checkout.

Each plugin instance owns its SPI descriptor, GPIO chips and lines, IRQ thread,
radio state and reassembly buffer. Multiple configured interfaces therefore do
not share mutable state. `rx_en` and `tx_en` are optional.

## Build

Dependencies: CMake 3.16+, a C11 compiler, pthreads, libm, libcyaml 2.x and
libgpiod 1.x. The libgpiod 2.x API is deliberately rejected at configure time.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

The installed filename is `sx1262.so`, without a `lib` prefix. The default
GNU installation path is `/usr/local/lib/reticulum-rs`; rsReticulum normally
loads `/usr/lib/reticulum-rs/sx1262.so`, so use `-DCMAKE_INSTALL_PREFIX=/usr`
when appropriate.

## Configuration

The plugin embeds an ABI 1.1 JSON Schema for its complete configuration. Its
readable source is `schema/sx1262.schema.json`; CMake converts it to a byte
array in the build directory and compiles that into `sx1262.so`, without extra
build-time tools. The rsReticulum web UI reads this metadata directly from the
library and builds the editor from it; there is no host-side fallback or
SX1262-specific form.

See `example.yaml`. Required plugin fields are `spi`, `cs`, `rst`, `busy`,
`dio1`, `frequency`, `bandwidth`, `spreading_factor`, `coding_rate` and
`tx_power`. Unknown keys are rejected.

Supported bandwidths are 7800, 10400, 15600, 20800, 31250, 41700, 62500,
125000, 250000 and 500000 Hz. Spreading factor is 5–12, coding rate is 4–8,
and TX power is 8–30 dBm (the PA setup follows the E22 M30S tuning used by
RNode-linux).

Optional fields and defaults:

| Field | Default |
| --- | ---: |
| `rx_en`, `tx_en` | absent |
| `preamble_symbols` | 25 |
| `sync_word` | `0x1424` (write `5156` in YAML) |
| `tcxo_voltage` | 1.8 V |
| `irq_watchdog_seconds` | 60 |
| `hard_reset_after` | 2 |

Allowed TCXO voltages are 1.6, 1.7, 1.8, 2.2, 2.4, 2.7, 3.0 and 3.3 V.

The receive watchdog derives separate PREAMBLE and HEADER deadlines from the
configured LoRa airtime. On expiry it first polls the SX1262 IRQ register, so a
packet is still handled when only the DIO1 edge was lost. With no pending IRQ,
the plugin restarts continuous RX without resetting the chip. After
`hard_reset_after` consecutive watchdog recoveries, or immediately after a
failed soft recovery or TX timeout, it toggles RST and reapplies the complete
radio configuration. A normal DIO1 event clears the recovery counter.

`irq_watchdog_seconds` additionally checks a radio which produces no DIO1
events at all. The valid range is 1–86400 seconds; `hard_reset_after` is 1–100.
During a hardware reset the interface transitions offline and returns online
only after radio initialization succeeds.

## Runtime behaviour

`send()` waits for physical `TX_DONE` for every LoRa fragment and uses finite
timeouts. The IRQ worker receives continuously and reports complete packets
with rounded RSSI and SNR metadata. `destroy()` first reports the interface
offline, stops and joins the IRQ worker, disables optional RF switches, and
releases every descriptor and GPIO resource.

Tests exercise framing/reassembly, strict YAML parsing and dynamic ABI loading
without radio hardware. SPI/GPIO operation and RF compatibility must also be
validated on the target board.

## Origin and license

The SX126x register sequence and RNode air framing are derived from
`RNode-linux`, copyright 2025 Belousov Oleg (R1CBU). This project is licensed
under LGPL-2.1-or-later, matching that source project. SPDX identifier:
`LGPL-2.1-or-later`.
