# OBS Multistream Plugin

A plugin for [OBS Studio](https://obsproject.com/) that lets you broadcast to **multiple RTMP/RTMPS destinations at the same time** (YouTube, Twitch, Kick) from a single OBS instance, without needing an external restreaming service.

The plugin adds a **Multistream Destinations** dock to OBS where you can configure additional platforms. When you click OBS's native **Start Streaming** button, OBS streams to its primary destination (configured in *Settings → Stream*) and this plugin streams in parallel to every additional destination you have enabled.

---

## Purpose

OBS Studio natively supports streaming to a single destination at a time. This plugin extends OBS so creators can:

- Go live on **YouTube, Twitch, Kick** (and any custom RTMP/RTMPS endpoint) simultaneously.
- Manage all destinations from a docked panel inside OBS — no external services, no second machine.
- Keep their existing OBS *Settings → Stream* configuration as the **primary** destination (locked, read-only in the plugin), and only manage the *additional* destinations from the dock.
- Choose a **video encoder per destination** from a platform-filtered list — the plugin only shows encoders compatible with each platform's codec requirements.
- Get **per-destination status** (Idle / Starting / Live / Failed / Stopped), error reporting, and automatic retries with exponential backoff on transient failures.

---

## Installation

### Requirements

- OBS Studio **30.0** or newer (Windows 64-bit).
- A working primary streaming destination in OBS *Settings → Stream*.

### Install the prebuilt release

1. Download the latest `obs-multistream-plugin-windows.zip` from [**GitHub Releases**](https://github.com/MRGuziX/obs_multistream_plugin/releases).
2. Close OBS Studio.
3. Extract the archive into your OBS installation folder (e.g. `C:\Program Files\obs-studio\`). The zip contains:
   ```
   obs-plugins/
       obs-multistream-plugin.dll
   data/
       obs-plugins/
           obs-multistream-plugin/
               locale/
                   en-US.ini
   ```
4. Start OBS. The new dock **Multistream Destinations** should be available under *View → Docks*.

### Build from source

See the [Build from source](#build-from-source-1) section at the end of this document.

---

## How the plugin works

### The dock UI

After installation, OBS shows a **Multistream Destinations** dock containing:

- An **Add destination** button — opens a modal dialog with platform, server, stream key, and video encoder fields.
- A **table** listing destinations with columns: *Enabled, Platform, Server, Encoder, Status, Last error*.
- **Edit selected** and **Remove selected** buttons (disabled while streaming).

### The locked "OBS default" row

The first row in the table mirrors the streaming service configured in OBS *Settings → Stream*. It is:

- **Pre-checked** and **always enabled** (OBS itself streams to it).
- **Locked / read-only** — you can't edit, uncheck, or remove it from the dock. Change it in *OBS Settings → Stream* instead.
- **Not persisted** to the plugin's JSON config — it's reconstructed from OBS at runtime.
- Shown in italic with a lock icon and a tooltip explaining where to edit it.

### Adding additional destinations

1. Click **Add destination** in the dock.
2. Pick a **platform** (YouTube, Twitch, Kick, or Other). The server URL fills in automatically for known platforms.
3. Enter your **stream key**.
4. Choose a **video encoder** from the filtered list (see Encoder handling below).
5. Click **Save**. The new row appears in the table with **Enabled** checked.

Validation rules:

- Platform, server and stream key are required.
- Server must start with `rtmp://` or `rtmps://`.
- Twitch / YouTube destinations must use `rtmps://`.
- Kick destinations must use port `:443`.
- Duplicate (same server + key) destinations are rejected.

Destinations are saved to `obs-multistream-plugin.json` in OBS's plugin config folder.

### Encoder handling

Understanding how encoders work is key to a smooth multistream experience.

#### What is an encoder?

An encoder compresses your raw video into a format that streaming platforms can receive. There are two main types:

- **Hardware encoders** (e.g., NVIDIA NVENC, AMD AMF, Intel QSV) — run on your GPU. Very fast, minimal CPU impact. But GPUs have a **session limit** — most consumer NVIDIA cards allow only **2–3 simultaneous NVENC sessions**.
- **Software encoders** (e.g., x264) — run on your CPU. No session limits, but uses CPU resources.

Each encoder produces video in a specific **codec** (compression format):

| Codec | Common encoders | Notes |
|-------|----------------|-------|
| **H.264** | NVIDIA NVENC H.264, Software x264, AMD H.264 | Universally supported by all platforms |
| **HEVC** | NVIDIA NVENC HEVC, AMD HEVC | Better quality at same bitrate, but **not supported by Twitch or Kick** |
| **AV1** | NVIDIA NVENC AV1, AMD AV1 | Newest, best compression, supported by YouTube only |

#### Platform codec restrictions

Not every platform accepts every codec. When you add a destination, the encoder dropdown **only shows compatible encoders**:

| Platform | Accepted codecs | Why |
|----------|----------------|-----|
| **YouTube** | H.264, HEVC, AV1 | Full codec support |
| **Twitch** | H.264 only | HEVC and AV1 streams will connect but get rejected |
| **Kick** | H.264 only | Same as Twitch |
| **Other** | All codecs | No filtering — you know your server's requirements |

This means if you select Twitch as the platform, you will **not** see NVIDIA NVENC HEVC in the encoder list — only H.264-compatible encoders appear. This prevents you from picking an encoder that the platform would reject.

Deprecated, fallback, and obsolete encoders are also hidden from the list.

#### Smart encoder session sharing

When you pick the **same encoder** for a secondary destination as your main OBS stream, the plugin does not create a new encoder instance. Instead, it **shares the existing encoder session** — the encoder runs once and OBS routes the same encoded data to multiple RTMP servers simultaneously.

This means:

- **Zero extra CPU/GPU cost** — no additional encoding work, just network output.
- **No GPU session limit issues** — one NVENC session serves all destinations that share it.
- **Same quality and bitrate** — all destinations sharing an encoder receive identical video.

When you pick a **different encoder**, the plugin creates a **dedicated instance** for that destination. This is useful when you want different quality/bitrate or need a different codec, but it does consume additional CPU or GPU resources.

#### Example setups

**Best setup — maximum efficiency (recommended):**

Set your main OBS stream (*Settings → Stream*) to **NVIDIA NVENC H.264**. Then add YouTube, Twitch, and Kick destinations — each also set to **NVIDIA NVENC H.264**. All four streams share **one** GPU encoder session. One NVENC session, all platforms, zero extra cost.

```
Main stream (YouTube)  → NVIDIA NVENC H.264  ← shared session
Twitch destination     → NVIDIA NVENC H.264  ← same session (shared)
Kick destination       → NVIDIA NVENC H.264  ← same session (shared)
```

**Mixed setup — different quality per destination:**

Main stream uses NVIDIA NVENC H.264 at high bitrate for YouTube. Twitch destination uses Software x264 at lower bitrate for a lighter stream. Two encoder instances: one GPU, one CPU.

```
Main stream (YouTube)  → NVIDIA NVENC H.264 (10,000 kbps)  ← GPU
Twitch destination     → Software x264 (6,000 kbps)         ← CPU
```

**What NOT to do:**

Setting the main stream to NVIDIA NVENC **HEVC** and a Twitch destination to NVIDIA NVENC **H.264** creates **two separate NVENC sessions** on your GPU. Most consumer GPUs will reject the second session. Twitch also cannot accept HEVC. Instead, use H.264 for the main stream so all destinations can share it, or use Software x264 for Twitch.

### Going live

When you click OBS's native **Start Streaming** button:

1. OBS starts its own primary stream (the locked row in the dock is marked **Live**).
2. One event-loop tick later, the plugin starts each **enabled** additional destination:
   - If the destination's encoder matches the main stream → **shares** the encoder session.
   - If different → **creates** a dedicated encoder instance (with x264 fallback if the requested encoder is unavailable).
3. Each destination's status transitions through *Starting → Live*. If a destination fails to start or drops, the plugin retries up to **3 times** with **2s / 4s / 6s** backoff.

### Stopping

When you click OBS's **Stop Streaming**, the plugin stops every active secondary output, releases each destination's output and service (and dedicated encoders if any), and clears pending retries.

### Streaming state protections

While streaming is active:

- **Edit** and **Remove** buttons are disabled — you cannot modify destinations mid-stream.
- **Enabled** checkboxes for active destinations are locked — you cannot disable a live output.
- All controls unlock when streaming stops.

---

## File layout

```
src/
    plugin-main.cpp                 - OBS module entry / lifecycle, frontend hooks
    plugin_state.{h,cpp}            - globals, runtime status, validation helpers
    dock_ui.{h,cpp}                 - dock UI (Qt), table, add/edit dialogs
    multistream_manager.{h,cpp}     - secondary outputs, encoder sharing, retries
    multistream_manager_signals.cpp - output signal handlers
    multistream_raii.h              - small OBS handle helpers
    config_io.{h,cpp}               - JSON load/save, sync with OBS default stream
    stream_key_storage.{h,cpp}      - optional OS key protection for saved keys
    destination_rules.{h,cpp}       - validation, normalization, platform codec filtering
    version.rc                      - Windows file version resource
tests/
    destination_rules_tests.cpp     - validation, normalization, platform codec tests
    config_io_tests.cpp             - config JSON round-trip / edge cases
    test_main.cpp                   - Catch2 runner
    *_stub.cpp                      - minimal stubs for test linking (no Qt)
data/locale/en-US.ini               - default English strings
CMakeLists.txt
```

---

## Troubleshooting

- **The dock is empty / no "OBS default" row appears.**
  Make sure you have a streaming service configured in *OBS Settings → Stream*. The plugin syncs the default row on `FINISHED_LOADING`, `PROFILE_CHANGED`, and `STREAMING_STARTING` events.
- **A destination is stuck in *Failed*.**
  Check the *Last error* column. Common causes: wrong stream key, server URL mismatch, network issue, or codec incompatibility. Fix the row via *Edit selected* and start streaming again.
- **Secondary stream connects but drops / reconnects.**
  The destination's encoder codec may not be accepted by the platform. For example, Twitch requires H.264 — if your main stream uses HEVC and the destination shares that encoder, Twitch will reject it. Set the destination's encoder to an H.264 encoder (e.g., Software x264 or NVIDIA NVENC H.264).
- **Multiple NVIDIA encoders fail to start.**
  Most consumer NVIDIA GPUs support only 2–3 simultaneous NVENC sessions. Use the **same encoder** as the main stream (shared session) or use **Software x264** for additional destinations.

---

## Known Limitations

- **Twitch bitrate cap (~8,000 kbps).** Twitch limits the maximum ingest bitrate per account — typically around 8,000 kbps for most accounts. When you share the main encoder, all destinations receive the same bitrate. If your OBS bitrate exceeds Twitch's limit, the stream connects but Twitch rejects the video data (error #1000 on the dashboard). **Fix:** set your OBS bitrate in *Settings → Output → Streaming* to 8,000 kbps or less. Both YouTube and Twitch will accept this — YouTube re-encodes on its side anyway, so the quality difference is negligible.
- **Encoder sharing requires compatible settings.** When sharing a single encoder across multiple platforms, all platforms must accept the encoder's settings (bitrate, keyframe interval, codec). If one platform has stricter requirements, lower the main encoder settings in *OBS Settings → Output* to the strictest common denominator.

---

## Build from source

**Prerequisites:** Windows, Visual Studio 2022 (C++ desktop workload), CMake 3.16+, Qt 6 Widgets.

**1. Fetch submodules**

```powershell
git submodule update --init --recursive
```

**2. Build OBS Studio from the submodule**

```powershell
cmake -S third_party/obs-studio -B build-obs `
  -DENABLE_PLUGINS=OFF `
  -DENABLE_SCRIPTING=OFF `
  -DCMAKE_INSTALL_PREFIX="$PWD/obs-install"
cmake --build build-obs --config Release
cmake --install build-obs --config Release
cmake --install build-obs --config Release --component Development
```

**3. Configure the plugin**

```powershell
$qt6Path = (Get-ChildItem "third_party/obs-studio/.deps/obs-deps-qt6-*" -Directory | Select-Object -First 1).FullName
$depsPath = (Get-ChildItem "third_party/obs-studio/.deps/obs-deps-20*" -Directory | Select-Object -First 1).FullName
cmake -B build -S . -DCMAKE_PREFIX_PATH="$PWD/obs-install;$PWD/obs-install/cmake;$qt6Path;$depsPath"
```

**4. Build and test**

```powershell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The built plugin is `build\Release\obs-multistream-plugin.dll`.

---

## Acknowledgements

This plugin was developed with the assistance of **Claude** (Anthropic), an AI coding agent. The author's primary background is in Python — Claude helped with C++ architecture, OBS API integration, encoder session management, CI/CD pipelines, and code review. All code was reviewed, tested, and validated manually by the author before release.

---

## License

See `COPYRIGHT` in the repository root.

## Contributing

See `CONTRIBUTORS.md`.
