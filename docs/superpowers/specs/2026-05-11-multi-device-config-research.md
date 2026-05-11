# Multi-device runtime config — research

**Status:** Research / proposal. No code changes in this branch.
**Goal:** Flash the same firmware binary to N devices. Each device runs its own Telegram bot without per-device recompilation.

## Problem statement

`include/secret.h` is the entire problem. It contains compile-time `#define`s consumed via macros throughout the codebase:

```c
#define SSID                      "..."
#define SSID_PASSWORD             "..."
#define OTA_PASSWORD              "..."
#define TELEGRAM_BOT_TOKEN        "..."
#define TELEGRAM_CHAT_ID          "..."
#define TELEGRAM_PROXY_BASE_URL   "..."
#define TELEGRAM_PROXY_AUTH_TOKEN "..."
```

The user has **2 devices** (likely growing), each pointed at a **different Telegram bot** (and therefore a different `TELEGRAM_BOT_TOKEN`). All other secrets are effectively shared:

| Secret | Per-device? | Why |
|---|---|---|
| `TELEGRAM_BOT_TOKEN` | **Yes** | Different bot per device |
| `TELEGRAM_CHAT_ID` | **Yes** | The bot DM's the same Telegram user, but the chat_id is bot-specific |
| `SSID` / `SSID_PASSWORD` | No | Same home WiFi |
| `OTA_PASSWORD` | Optional | Could be shared; per-device adds tiny security benefit |
| `TELEGRAM_PROXY_*` | No | Same Cloud.ru proxy for all devices |

The minimal change: **move `TELEGRAM_BOT_TOKEN` and `TELEGRAM_CHAT_ID` out of `secret.h` into runtime config**. Everything else stays compile-time.

## Reference map

All call sites live in `include/TelegramNotifier.h` (12 + 6 references for bot_token + chat_id respectively). Pattern is consistent:

```c
String body = "bot_token=" + urlEncode(String(TELEGRAM_BOT_TOKEN)) +
              "&chat_id=" + urlEncode(String(TELEGRAM_CHAT_ID)) + ...
String url  = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN +
              "/sendMessage?chat_id=" + TELEGRAM_CHAT_ID + ...
```

Either macro is sometimes used as a `const char*` (URL concatenation) and sometimes as a `String` argument (form body). Both forms work with a `String` runtime variable, so the refactor is search-and-replace — no API shape changes.

## Options considered

### A. Full LittleFS-backed `RuntimeConfig` (every secret moves to runtime)

**Sketch.** New `RuntimeConfig` struct mirroring `Settings`. Persist to `/config.json` on LittleFS. First boot with no config → AP mode (captive portal) for WiFi + bot provisioning. WiFi creds also runtime.

- **Pros**: Truly device-agnostic firmware. New device → flash → connect to AP → provision.
- **Cons**: Major refactor. Needs AP-mode captive portal (~500 lines). WiFi state machine becomes "either AP or STA based on config presence." Boot flow doubles in complexity.

### B. Hybrid: only the per-device secrets go to runtime (RECOMMENDED)

**Sketch.** New `DeviceConfig` struct with two fields: `telegram_bot_token`, `telegram_chat_id`. Persist to `/device_config.json` on LittleFS.

Boot flow:
1. `setup()` loads `/device_config.json`. If missing → device boots with empty bot token.
2. WiFi connects (creds still from `secret.h`). Web UI + API come up normally.
3. `TelegramNotifier::ensureBotCommandsRegistered()` and `checkForCommands()` early-return if `g_device_cfg.telegram_bot_token.isEmpty()` — Telegram side stays silent.
4. User browses to `http://<device-ip>/setup` (new page), enters bot token + chat_id, submits.
5. API endpoint `POST /api/device_config` validates length (>20 chars, contains `:` for bot tokens), saves to `/device_config.json`, updates the in-memory `g_device_cfg`, calls `ensureBotCommandsRegistered()` immediately.
6. User sends `/start` to their bot — device responds. Provisioning done.

Code changes:
- New `include/DeviceConfig.h` (~80 lines, JSON I/O modeled on `Settings.h`)
- `TelegramNotifier.h`: replace every `TELEGRAM_BOT_TOKEN` / `TELEGRAM_CHAT_ID` macro with `g_device_cfg.telegram_bot_token.c_str()` / `g_device_cfg.telegram_chat_id.c_str()`. Add an `isConfigured()` short-circuit at the top of the polling and send-message functions.
- `src/main.cpp`: load `/device_config.json` in `setup()`, define `g_device_cfg` global.
- `api_handlers.h`: `GET /api/device_config` (returns `{configured: bool}` — never returns the token itself), `POST /api/device_config` (validates + persists + reloads).
- `data/web/prod/`: new `/setup` page or banner on the existing index when `!configured`.
- `secret.h.template`: stripped of `TELEGRAM_BOT_TOKEN` and `TELEGRAM_CHAT_ID` (still has SSID, OTA password, proxy URL/token).

Estimated diff: ~300 LOC across firmware, ~50 LOC in web UI.

- **Pros**: Surgical. Backward compat is trivial (devices with no config file just have Telegram disabled — no crash). LittleFS persists across `pio run -t upload` (only `uploadfs` wipes it, verified in this session).
- **Cons**: Requires a one-time web-UI provisioning step per device after first flash. WiFi creds still need a code change if you ever add a device on a different network — but that's the same as today.

### C. Multi-bot in one firmware, device picks via MAC address

**Sketch.** `secret.h` defines a **table** of `{mac, bot_token, chat_id}` tuples. Boot reads `WiFi.macAddress()`, looks up its own entry, uses that for Telegram.

```c
struct DeviceProfile { const char* mac; const char* bot_token; const char* chat_id; };
static const DeviceProfile DEVICES[] = {
    { "AA:BB:CC:DD:EE:01", "111:AAAA...", "12345" },
    { "AA:BB:CC:DD:EE:02", "222:BBBB...", "67890" },
};
```

- **Pros**: Zero runtime config UX. Flash and done.
- **Cons**: Adding device #3 means a code change + reflash of *all* devices. Tokens still in source (or `secret.h`), which the user wants to avoid. Doesn't scale beyond a small fleet.

### D. Build-time variants

**Sketch.** PlatformIO env per device: `[env:device1]`, `[env:device2]`, each picking a different `secret.h`. Different binaries.

- **Pros**: Minimal code change.
- **Cons**: This is essentially what the user is doing today and explicitly rejected. Two firmware binaries.

## Recommendation

**Option B (Hybrid: per-device secrets → LittleFS).**

Right balance of effort and clean-ness. The user's complaint is specifically about the Telegram token, and B is the smallest refactor that fully solves it. The WiFi creds staying in `secret.h` is fine because they're already shared.

If, later, the user wants a 3rd device on a different network, the migration to "WiFi creds also runtime" (Option A scope creep) is mechanical — same pattern, same Settings-style JSON, just adds an SSID field. Don't pay for it today.

## Open questions before implementation

1. **First-boot UX**: should the web UI show a dedicated `/setup` page, or just a banner on the existing index page that links to a settings form? I'd argue *banner on index* — fewer routes.
2. **API response for already-configured devices**: `GET /api/device_config` returns `{configured: true, bot_token_preview: "XXX...:****"}` (masked) or just `{configured: true}` (opaque)? Opaque is safer; preview helps the user remember which bot is which.
3. **`/reset_device_config` endpoint**: useful if user wants to repoint a device to a different bot. Should require OTA password auth.
4. **Migration for existing devices**: on first boot of the new firmware on a device that previously had `TELEGRAM_BOT_TOKEN` baked in, the token would suddenly be empty. Two options: (a) one-time bootstrap — if no `/device_config.json` AND a compile-time `TELEGRAM_BOT_TOKEN` is defined, write that to LittleFS on first boot and clear the macro path on subsequent boots. (b) ignore — user re-provisions via web UI. I'd pick (a) for the smoother transition.
5. **Should `OTA_PASSWORD` also become per-device?** Currently shared. Per-device is more secure (compromise of one device's OTA pw doesn't compromise others). Out of scope for this change, but worth flagging as a follow-up.

## Implementation plan (when approved)

1. Create `include/DeviceConfig.h` modeled on `Settings.h` (defaults, fromJson, toJson, load, save, isConfigured).
2. Define `g_device_cfg` global in `src/main.cpp`. Load in `setup()` after LittleFS init.
3. Search-and-replace `TELEGRAM_BOT_TOKEN` and `TELEGRAM_CHAT_ID` in `TelegramNotifier.h` with `g_device_cfg.telegram_bot_token.c_str()` / `.telegram_chat_id.c_str()`. Add `isConfigured()` guards in `sendMessage`, `sendNotificationMessage`, `checkForCommands`, `ensureBotCommandsRegistered`, `answerCallbackQuery`.
4. Add `GET`/`POST /api/device_config` to `api_handlers.h`.
5. Web UI: setup banner on index when `!configured`, form posts to `/api/device_config`.
6. Bootstrap shim: if compile-time `TELEGRAM_BOT_TOKEN` is non-empty AND `/device_config.json` doesn't exist, seed it on first boot. (Lets the user upgrade in place.)
7. Update `secret.h.template` — remove the two macros, add a note pointing to the web UI.
8. Update `CLAUDE.md` and `MOTHER_PROJECT.md`: document the runtime config pattern.
9. Add native unit tests for `DeviceConfig::fromJson` / `toJson` / range validation.
10. Bump version → `1.1.0` (minor bump — semantic change to provisioning model).

## Notes on scope

- This research stays **firmware-internal**. The Cloud.ru side (`telegram_bot_api_proxy.py`, `esp32_metrics_proxy.py`) is bot-token-agnostic — it forwards whatever token the device sends. No server-side change.
- The two devices currently flashed with v1.0.6 share `TELEGRAM_BOT_TOKEN` from `secret.h`. After implementation: same binary, but each device's `/device_config.json` differs. Bootstrap shim (point 6 above) means existing devices continue to work without manual re-provisioning if the user keeps `TELEGRAM_BOT_TOKEN` defined in `secret.h` for the initial migration.
