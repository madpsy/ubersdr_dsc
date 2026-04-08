# ubersdr_dsc

Multi-frequency DSC (Digital Selective Calling) decoder for [UberSDR](https://ubersdr.org) — connects to a remote UberSDR instance, simultaneously monitors up to 25 ITU-R M.541 DSC frequencies, decodes DSC messages (ITU-R M.493), and serves decoded messages in a live web UI.

---

## How it works

```
UberSDR (remote SDR) ──► dsc_rx_from_ubersdr (C++) ──► decoded DSC messages
        │                        │
        │  N parallel WebSocket  │
        │  connections (one per  └──► web UI  http://<host>:6093
        │  frequency channel)
        │
        ├── WS freq=2177000 ──► Decoder ──┐
        ├── WS freq=2189500 ──► Decoder ──┤
        ├── WS freq=4208000 ──► Decoder ──┼──► Shared Message Store ──► Web UI
        ├── WS freq=...     ──► Decoder ──┤
        └── WS freq=25209500──► Decoder ──┘
```

- **`dsc_rx_from_ubersdr`** — C++ service that opens N parallel WebSocket connections to UberSDR (one per frequency), each with its own independent FSK demodulator and DSC decoder. Decoded messages from all channels feed into a shared message store and are broadcast to browser clients via a real-time web UI.
- **Web UI** — live decoded DSC message table with MMSI/country lookups, per-frequency metrics dashboard, and audio preview; served on port 6093.

---

## Quick start (Docker — recommended)

```bash
curl -fsSL https://raw.githubusercontent.com/madpsy/ubersdr_dsc/master/install.sh | bash
```

This will:
1. Create `~/ubersdr/dsc/` and download `docker-compose.yml` + helper scripts
2. Pull the latest `madpsy/ubersdr_dsc` image
3. Start the service

Then edit `~/ubersdr/dsc/docker-compose.yml` to set your UberSDR URL and frequencies, and run `./restart.sh`.

---

## Configuration

All configuration is via environment variables in `docker-compose.yml`:

| Variable | Default | Description |
|----------|---------|-------------|
| `UBERSDR_URL` | `http://ubersdr:8080` | UberSDR base URL |
| `DSC_FREQS` | `2187500` | DSC frequencies in Hz, comma-separated (see [DSC frequencies](#dsc-frequencies) below) |
| `WEB_PORT` | `6093` | Web UI port |

### CLI arguments

When running the binary directly:

```
dsc_rx_from_ubersdr [--sdr-url URL] [--web-port N] [--freqs f1,f2,...]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--sdr-url URL` | `http://localhost:8073` | UberSDR server base URL |
| `--web-port N` | `6093` | Local web UI port |
| `--freqs f1,f2,...` | all 25 ITU frequencies | Comma-separated frequencies in Hz |

Examples:

```bash
dsc_rx_from_ubersdr --sdr-url http://192.168.1.10:8073
dsc_rx_from_ubersdr --freqs 2187500,8414500,16804500
dsc_rx_from_ubersdr --sdr-url http://sdr:8073 --web-port 8080 --freqs 2187500
```

---

## Helper scripts

After running `install.sh`, the following scripts are available in `~/ubersdr/dsc/`:

| Script | Action |
|--------|--------|
| `./start.sh` | Start the service |
| `./stop.sh` | Stop the service |
| `./restart.sh` | Restart the service (apply config changes) |
| `./update.sh` | Pull the latest image and restart |

---

## Building from source

### Docker image

```bash
docker build -t madpsy/ubersdr_dsc:latest .
```

### Local build (no Docker)

Requires: `build-essential`, `cmake`, `libzstd-dev`, `libcurl4-openssl-dev`, `libssl-dev`, `pkg-config`

IXWebSocket is cloned automatically from GitHub if not present.

```bash
./build.sh
# Binary: ./build/src/dsc_rx_from_ubersdr
```

Use `./build.sh --clean` to remove the build directory before rebuilding.

---

## Web UI

Open `http://<host>:6093` in a browser. The UI has three tabs:

### Messages

The default view showing all decoded DSC messages across all frequencies:

- Real-time decoded DSC message table with columns: Time, Frequency, Format, Category, Self ID, Country, Address, Address Country, TC1, TC2, Distress, Position, EOS, ECC
- MMSI → country and coast station name lookups
- Frequency filter dropdown — show messages from a specific frequency or all
- Category filter — Distress, Urgency, Safety, Routine
- Valid-only toggle — hide messages with ECC errors
- Distress highlighting — distress calls shown with red background, urgency with orange, safety with yellow
- Click any row to expand full JSON details

### Frequency Monitor

Per-channel RF analysis dashboard:

- One row per configured frequency with: Frequency (MHz), Band, Connection status, Message count, Valid messages, Errors, Error rate, Last message time, Average RSSI, Reconnect count
- Color-coded status: green = active (messages received), yellow = connected but idle, red = disconnected, grey = disabled
- Click any frequency to jump to the Messages tab filtered to that frequency

### Audio Preview

- Select which frequency channel to listen to
- Stream raw SDR audio to the browser
- Audio level meter

---

## Ports

| Port | Description |
|------|-------------|
| `6093` | Web UI (HTTP + WebSocket) |

---

## DSC frequencies

All 25 ITU-R M.541 DSC frequencies (USB dial frequencies):

| Frequency (Hz) | MHz | Band |
|----------------|-----|------|
| `2177000` | 2.1770 | MF |
| `2189500` | 2.1895 | MF |
| `4208000` | 4.2080 | 4 MHz |
| `4208500` | 4.2085 | 4 MHz |
| `4209000` | 4.2090 | 4 MHz |
| `6312500` | 6.3125 | 6 MHz |
| `6313000` | 6.3130 | 6 MHz |
| `8415000` | 8.4150 | 8 MHz |
| `8415500` | 8.4155 | 8 MHz |
| `8416000` | 8.4160 | 8 MHz |
| `12577500` | 12.5775 | 12 MHz |
| `12578000` | 12.5780 | 12 MHz |
| `12578500` | 12.5785 | 12 MHz |
| `16805000` | 16.8050 | 16 MHz |
| `16805500` | 16.8055 | 16 MHz |
| `16806000` | 16.8060 | 16 MHz |
| `18898500` | 18.8985 | 18 MHz |
| `18899000` | 18.8990 | 18 MHz |
| `18899500` | 18.8995 | 18 MHz |
| `22374500` | 22.3745 | 22 MHz |
| `22375000` | 22.3750 | 22 MHz |
| `22375500` | 22.3755 | 22 MHz |
| `25208500` | 25.2085 | 25 MHz |
| `25209000` | 25.2090 | 25 MHz |
| `25209500` | 25.2095 | 25 MHz |

When no `--freqs` / `DSC_FREQS` is specified, all 25 frequencies are monitored by default. Each frequency opens its own WebSocket connection to the UberSDR server.

---

## File-based testing

The `dsc_rx_from_file` tool decodes DSC messages from raw PCM audio files (16-bit signed little-endian mono):

```
dsc_rx_from_file [options] <filename>
dsc_rx_from_file [options] -          (read from stdin)
```

| Option | Default | Description |
|--------|---------|-------------|
| `--sample-rate <hz>` | `12000` | Input sample rate |
| `--frequency <hz>` | `0` | Frequency label for display |
| `--json` | off | Output as JSON (one message per line) |
| `--verbose` | off | Print decoder progress info |

Examples:

```bash
# Decode from a WAV-like raw PCM file
./build/src/dsc_rx_from_file --sample-rate 12000 recording.raw

# Pipe from sox or similar
sox input.wav -t raw -r 12000 -e signed -b 16 -c 1 - | ./build/src/dsc_rx_from_file --json -

# With frequency label and verbose output
./build/src/dsc_rx_from_file --frequency 2187500 --verbose --json recording.raw
```

---

## Credits

- Jon Beniston, M7RCE / [SDRangel](https://github.com/f4exb/sdrangel) — DSC decoder and message parser (`DSCDecoder`, `DSCMessage`, MMSI utilities, coast station database)
- [ubersdr_navtex](https://github.com/madpsy/ubersdr_navtex) — FSK demodulator, ubersdr WebSocket protocol integration, web UI architecture
- Dave Freese, W1HKJ / [fldigi](http://www.w1hkj.com/) — FFT overlap-add filters (`fftfilt`)

---

## License

Licensed under the GNU GPL V3. See [LICENSE](LICENSE) for details.
