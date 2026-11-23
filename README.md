# Clam Build

A native Windows desktop coding agent — one `.exe`, minimal and themeable, no Python or terminal required.

[Download the latest Windows build](https://github.com/lettermantest/clam-build/releases/latest) — 3.4 MB, no installer, no runtime dependencies.

---

## What the screenshot showed

`(empty response)` was a real bug, now fixed.

**Cause:** reasoning models sometimes return a message whose visible `content` is empty while all the text goes into `reasoning_content`. Worse, that empty assistant turn was appended to conversation history, and once an empty assistant message is in history the model refuses to answer — so every later turn also came back blank. That is why "ok so do it" produced nothing.

**Fix (three layers):**

1. **History sanitization** — empty assistant turns are dropped before every request, and empty tool results become `(tool returned no output)`.
2. **Staged recovery** — if a reply has no visible text, Clam retries automatically: normal retry → retry without tools → retry with double the token budget. If everything still fails it shows the model's reasoning text rather than a blank bubble, or a clear error telling you to lower Effort or switch models.
3. **Network retries** — transient socket/DNS errors, 429s and 5xx responses retry up to 3 times instead of silently failing.

Verified against the live API with the exact poisoned-history case from your screenshot: returns a full answer instead of nothing.

---

## Using it

Run `Clam Build.exe` from anywhere. Settings and your API key live in `%APPDATA%\clam`, so the exe is portable.

1. **Settings** → paste your NVIDIA API key → Save. Stored with Windows DPAPI.
2. **Settings** → Project folder → Browse / Open.
3. Type in the composer, press **Enter** to send (**Shift+Enter** for a newline).

### Interface

| Area | Notes |
|---|---|
| **Chat** | Assistant replies in cards, your messages right-aligned. Tool calls never clutter the transcript. |
| **Activity** | Collapsed rows like `▸ file change · Wrote index.html` — click to expand the full output. |
| **Settings** | API key, project folder, model list, reasoning effort, permissions, plugins. |
| Header | Segoe UI wordmark, rounded pill tabs, then quiet text chips — model, effort, permissions, New chat. |
| Sidebar | Saved-chat rail. It hides one second after the pointer leaves it; hover the slim left-edge grip to bring it back. Hover a tile for the full title/project/age, and scroll the rail with the mouse wheel. |

Every conversation is saved as a JSON file in `%APPDATA%\clam\chats` after each message or tool step, so saved chats reappear in the sidebar after a restart. Select a tile to reopen it; the active conversation is restored on launch.

The model chooser is an overlay popup: click the model name in the header (or the field in Settings). Navigate with **↑/↓**, **PgUp/PgDn**, **Home/End**, or the mouse wheel; **Enter** selects, **Esc** or clicking outside closes it. Vision-capable models are marked `◉`. Your choice saves immediately and is restored next launch.

Keyboard: **Ctrl+1/2/3** switch Chat/Activity/Settings, **Ctrl+V** pastes a clipboard screenshot, **Ctrl+N** new chat, **Esc** cancels a running turn.

### Images

Copy a screenshot and press **Ctrl+V** (or click Paste). Up to 10 per message, shown as chips in the composer and sent as `[image 1]`, `[image 2]`… You can also drag image files onto the window.

### Models and effort

The model list contains every code-capable NVIDIA NIM model available to your key (58 on this machine), hiding embedding, rerank, detector, speech, image-generation and other non-coding endpoints.

Effort levels map to reasoning budget:

| Effort | max_tokens | reasoning |
|---|---|---|
| low | 2048 | low |
| medium | 4096 | medium |
| high | 8192 | high |
| max | 16384 | high |

**If a reply ever comes back blank, set effort to `low`** — reasoning models that spend a whole budget thinking are the usual culprit.

### Permissions

Defaults to **Allow all**: commands and file writes run without prompting. Click the permission chip (or `● Allow all` in the header) to switch to **Manual**, which asks before run commands and overwrites.

---

## Capabilities

The agent has 14 tools:

| Tool | Purpose |
|---|---|
| `read_file` / `write_file` / `list_files` / `search_text` | Local files anywhere on the PC, absolute or relative |
| `run_command` | Shell commands in the project folder |
| `web_search` / `fetch_url` | Search and read documentation |
| `visual_test` / `view_image` | Headless Chrome screenshot, attach images |
| `system_info` | OS, CPU, RAM, GPU, serial ports |
| `arduino_boards` / `arduino_upload` / `arduino_setup` / `serial_read` | ESP32 and Arduino boards via arduino-cli |

---

## Plugins

Drop `.luau` scripts in `%APPDATA%\clam\plugins`. Each declares a name, description and handler; loaded plugins appear in Settings → Plugins.

---

## Building from source

Requires a C++20 compiler. Tested with MinGW-w64 GCC 16.2:

```powershell
cd cpp
windres clam.rc -O coff -o clam.res
g++ -std=c++20 -O2 -municode -DUNICODE -D_UNICODE `
    main.cpp json.cpp http.cpp config.cpp agent.cpp ui.cpp clam.res `
    -o "..\dist-cpp\Clam Build.exe" -mwindows `
    -lcomctl32 -lole32 -loleaut32 -lshell32 -lshlwapi -lcrypt32 `
    -lwinhttp -lgdi32 -luser32 -luuid -lgdiplus -lmsimg32 -ldwmapi `
    -static -static-libgcc -static-libstdc++
```

No third-party libraries: JSON parsing, HTTPS (WinHTTP), DPAPI, and the entire UI are written against the Win32 API. Total ~5,100 lines across 6 translation units.

### Source layout

| File | Contents |
|---|---|
| `json.h/.cpp` | JSON value type, parser, serializer, UTF-8 conversion |
| `http.h/.cpp` | WinHTTP client |
| `config.h/.cpp` | Settings, DPAPI key storage, model filters, plugins |
| `agent.h/.cpp` | Tool execution and the model turn loop |
| `ui.h/.cpp` | Custom-drawn themes, rounded GDI controls, input handling |
| `main.cpp` | Entry point |

---

## Troubleshooting

**Blank reply** — set Effort to `low`, or pick another model.

**"Add your NVIDIA API key in Settings"** — paste the key and press Save. Keys from the earlier Python build are read automatically (hex DPAPI + UTF-16 decoding handled).

**Model list empty** — click Refresh in Settings; it needs a valid key and network access.

**Arduino upload fails** — install `arduino-cli` and the board core (`arduino-cli core install esp32:esp32`).
