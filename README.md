# OBS Multistream Plugin

A plugin for [OBS Studio](https://obsproject.com/) that lets you broadcast to **multiple RTMP/RTMPS destinations at the same time** (YouTube, Twitch, Kick) from a single OBS instance, without needing an external restreaming service.

The plugin adds a **Multistream Destinations** dock to OBS where you can configure additional platforms. When you click OBS's native **Start Streaming** button, OBS streams to its primary destination (configured in *Settings → Stream*) and this plugin streams in parallel to every additional destination you have enabled.

---

## Purpose

OBS Studio natively supports streaming to a single destination at a time. This plugin extends OBS so creators can:

- Go live on **YouTube, Twitch, Kick** (and any custom RTMP/RTMPS endpoint) simultaneously.
- Manage all destinations from a docked panel inside OBS — no external services, no second machine.
- Keep their existing OBS *Settings → Stream* configuration as the **primary** destination (locked, read-only in the plugin), and only manage the *additional* destinations from the dock.
- Use **vertical scaling** per destination (e.g., 720x1280) for platforms that prefer a portrait feed.
- Get **per-destination status** (Idle / Starting / Live / Failed / Stopped), error reporting, and automatic retries with exponential backoff on transient failures.

---

## Installation

### Requirements

- OBS Studio **30.0** or newer (Windows 64-bit).
- A working primary streaming destination in OBS *Settings → Stream*.

### Install the prebuilt release

1. Download the latest `obs-multistream-plugin-release.zip` from the `dist/` folder of this repository (or from the project's Releases page if available).
2. Close OBS Studio.
3. Extract the archive. Inside you will find:
   ```
   obs-plugins/
       obs-multistream-plugin.dll
   ```
4. Copy `obs-multistream-plugin.dll` into your OBS plugins directory:
   ```
   C:\Program Files\obs-studio\obs-plugins\64bit\
   ```
   (or wherever OBS is installed on your system).
5. Start OBS. The new dock **Multistream Destinations** should be available under *View → Docks*.

### Build from source

Prerequisites: Windows, Visual Studio 2022 (with C++ workload), CMake 3.20+, and the OBS Studio SDK headers and import libraries (already vendored under `third_party/` for this project).

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The built plugin will be at `build\Release\obs-multistream-plugin.dll`. Copy it into OBS's `obs-plugins\64bit\` directory as described above.

To run the unit tests:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

---

## How the plugin works

### The dock UI

After installation, OBS shows a **Multistream Destinations** dock containing:

- A **Platform / Server / Stream key** form to add a new destination.
  - Selecting a platform (YouTube / Twitch / Kick) auto-fills a sensible default RTMP/RTMPS server.
- An **Add destination** button.
- A **table** listing all configured destinations with columns:
  *Enabled, Platform, Server, Protocol, Vertical, Status, Last error*.
- **Edit selected** and **Remove selected** buttons.

### The locked "OBS default" row

The first row in the table mirrors the streaming service configured in OBS *Settings → Stream*. It is:

- **Pre-checked** and **always enabled** (OBS itself streams to it).
- **Locked / read-only** — you can't edit, uncheck, or remove it from the dock. Change it in *OBS Settings → Stream* instead.
- **Not persisted** to the plugin's JSON config — it's reconstructed from OBS at runtime.
- Shown in italic with a 🔒 (OBS default) suffix and a tooltip explaining where to edit it.

This guarantees you always know what OBS itself will broadcast to, and prevents accidentally adding a duplicate of the OBS-primary destination.

### Adding additional destinations

1. Pick a platform from the dropdown (YouTube, Twitch, or Kick — the server URL fills in automatically). For other platforms, you can edit a row and replace the server with any valid `rtmp://` or `rtmps://` URL.
2. Paste your **stream key** for that platform.
3. Click **Add destination**. The row appears in the table with **Enabled** checked.
4. Toggle the **Enabled** checkbox per row to control which destinations go live next time you start streaming.

Validation rules enforced when adding/editing:

- Platform, server and stream key are required.
- Server must start with `rtmp://` or `rtmps://`.
- Twitch / YouTube destinations must use `rtmps://`.
- Kick destinations must use port `:443`.
- Duplicate (same server + key) destinations are rejected.

Destinations are saved to `obs-multistream-plugin.json` in OBS's plugin config folder.

### Going live

When you click OBS's native **Start Streaming** button:

1. OBS starts its own primary stream (the locked row in the dock is marked **Live**).
2. One event-loop tick later, the plugin starts each **enabled** additional destination as a separate `rtmp_output` with **its own dedicated video and audio encoders** (`obs_x264` + `ffmpeg_aac`) bound to OBS's global video/audio. Each destination owns its encoders — they are never shared with the main OBS output, which avoids the use-after-free / encoder-reuse crash that would otherwise happen.
3. Each destination's status transitions through *Starting → Live*. If a destination fails to start or drops, the plugin retries up to **3 times** with **2s / 4s / 6s** backoff. After the retry budget is exhausted the destination is marked **Failed** with a terminal reason in the *Last error* column.
4. Vertical-flagged destinations are scaled to **720×1280** by their dedicated encoder.

### Stopping

When you click OBS's **Stop Streaming**, OBS fires `STREAMING_STOPPING` / `STREAMING_STOPPED`. The plugin then stops every active secondary output, releases each destination's output, service, and encoders, and clears any pending retries.

### Reliability notes

- Each destination has its **own** service, output, video encoder, and audio encoder — these are tracked in a `DestinationRuntime` and released cleanly on stop or on failed start.
- Secondary outputs are started one event-loop tick **after** OBS reports `STREAMING_STARTED`, to avoid racing OBS while it wires up its main pipeline.
- The locked "OBS default" row is **never** started by the plugin — OBS owns that stream; the plugin only mirrors its Live/Stopped status in the table.
- The plugin keeps signal-handler lifetimes tight: callbacks are connected on start and disconnected on stop / cleanup, so output signals can never fire into a destroyed runtime.

### File layout

```
src/
    plugin-main.cpp        - OBS module entry point, dock UI, MultistreamManager
    destination_rules.cpp  - validation, normalization, platform detection
    destination_rules.h
tests/
    destination_rules_tests.cpp
third_party/                - vendored OBS SDK headers and import libs
dist/
    obs-multistream-plugin/         - install layout (obs-plugins/...)
    obs-multistream-plugin-release.zip
CMakeLists.txt
```

---

## Troubleshooting

- **The dock is empty / no "OBS default" row appears.**
  Make sure you have a streaming service configured in *OBS Settings → Stream*. The plugin retries reading it at 0 / 250 / 750 / 2000 / 5000 ms after the dock loads, plus on every `FINISHED_LOADING` / `PROFILE_CHANGED` / `STREAMING_STARTING` event.
- **A destination is stuck in *Failed* with a terminal reason.**
  Check the *Last error* column. Common causes: wrong stream key, server URL doesn't match the platform's required scheme/port, network issue. Fix the row via *Edit selected* and start streaming again.
- **OBS crashes on Start Streaming with destinations enabled.**
  This was a known issue caused by sharing the main streaming output's encoders with secondary outputs. It is fixed in current builds — every destination now owns its encoders. Make sure you are running the latest plugin DLL.

---

## License

See `COPYRIGHT` (and `LICENSE` if present) in the repository root.

## Contributing

See `CONTRIBUTORS.md`.
