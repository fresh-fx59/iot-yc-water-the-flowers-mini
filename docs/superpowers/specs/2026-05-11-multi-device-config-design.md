# Multi-device Telegram bot tokens — design

**Status:** Approved by user. Ready to hand off to writing-plans.
**Prior docs:** Built on top of `2026-05-11-multi-device-config-research.md` (options A/B/C/D survey) and `2026-05-11-multi-device-config-followup-mac-table.md` (rationale for MAC-keyed table after user clarified chat_id is shared).

## Goal

Flash the same firmware binary to every device in the user's fleet. Each device runs its own Telegram bot without per-device source edits at flash time, and without per-device manual provisioning after a fresh flash.

## Design contract

**Single-line model:** *secret.h is the factory default; LittleFS is what the device remembers.*

The resolver consults LittleFS first. If a previously-persisted token exists, use it. Otherwise look up the device's MAC in `secret.h`'s `DEVICE_TOKENS[]` table and write the matching token to LittleFS on first hit. Fall back to the compile-time `TELEGRAM_BOT_TOKEN` macro for unknown MACs (dev boards, manual swaps) — empty string disables Telegram silently.

## Components

### 1. `include/DeviceToken.h` (new, ~80 LOC)

```cpp
namespace DeviceToken {
    // Cached resolved token. Resolved once at boot; stable thereafter
    // unless reset() clears the LittleFS file.
    inline String& cachedToken();
    inline String& cachedChatId();

    // Called once during setup() after LittleFS is mounted and WiFi has
    // a MAC. Reads /device_config.json or bootstraps from secret.h's table.
    // Returns true if a usable token was resolved.
    bool init();

    // Read-only accessors used by TelegramNotifier in place of macros.
    const char* token();
    const char* chatId();
    bool        isConfigured();

    // Overwrite the persisted config and reboot (so the new token takes
    // effect). `set_by` is recorded in JSON for debug visibility.
    bool overwrite(const String& bot_token, const String& chat_id,
                   const char* set_by);  // "api" | "telegram"

    // Erase /device_config.json. Next boot re-bootstraps from secret.h.
    bool reset();
}
```

### 2. `include/secret.h` (modified)

The legacy macro stays as a fallback for unrecognized MACs. New: a sized `DeviceTokenEntry` table.

```cpp
// Legacy fallback — used when MAC is unknown and no /device_config.json exists.
#define TELEGRAM_BOT_TOKEN     "..."   // first-bot token
#define TELEGRAM_CHAT_ID       "..."   // shared across fleet

struct DeviceTokenEntry {
    const char* mac;          // upper-case "AA:BB:CC:DD:EE:FF"
    const char* bot_token;
    const char* label;        // human-readable, for diagnostics only
};

static const DeviceTokenEntry DEVICE_TOKENS[] = {
    { "94:A9:90:D2:00:8C", "8631563179:AAE711...", "watering bot 2 palm" },
    // { "??:??:??:??:??:??", "8783982519:AAG8sK...", "watering bot 1" },
};
static const size_t DEVICE_TOKEN_COUNT = sizeof(DEVICE_TOKENS)/sizeof(DEVICE_TOKENS[0]);
```

chat_id stays as the existing macro — it's shared.

### 3. `/device_config.json` (LittleFS, atomic write)

```json
{
  "bot_token": "8631563179:AAE711...",
  "chat_id": "314102923",
  "set_by": "bootstrap" | "api" | "telegram",
  "set_unix": 1778473213
}
```

Set via the same atomic temp-file + rename pattern used by `Settings.h`. `set_by` and `set_unix` are debug-only — they appear in `GET /api/device_config` and the `/status` Telegram dump.

### 4. `include/TelegramNotifier.h` (modified)

Search-and-replace: every `TELEGRAM_BOT_TOKEN` macro reference (12 sites) becomes `DeviceToken::token()`. Every `TELEGRAM_CHAT_ID` reference (~6 sites) becomes `DeviceToken::chatId()`. Both return `const char*` — same shape as the macro, no caller changes beyond the substitution.

Three top-level functions get an `isConfigured()` short-circuit (return early if no token resolved):
- `sendMessage()` / `sendNotificationMessage()` — drop on the floor with a serial-only log line.
- `checkForCommands()` — skip the long-poll entirely; nothing to receive on.
- `ensureBotCommandsRegistered()` — skip; nothing to register against.

### 5. `include/api_handlers.h` (modified, +3 endpoints)

```
GET  /api/device_config        → {"configured": true,
                                  "label": "watering bot 2 palm",
                                  "bot_token_preview": "********...8c-iLbJU",
                                  "chat_id": "314102923",
                                  "set_by": "bootstrap",
                                  "set_unix": 1778473213}
POST /api/device_config        → body: {"bot_token": "...", "chat_id": "..."}
                                  validates, writes, reboots
POST /api/device_config/reset  → erases /device_config.json, reboots
```

Both write endpoints validate: bot_token ≥ 30 chars and contains `:`; chat_id is digits-only. 400 on validation failure, 200 + reboot on success. No HTTP-basic-auth on these (LAN-only ESP32, OTA already trusts LAN traffic). Token preview is the last 5 chars only — enough for the operator to disambiguate two bots, not enough to leak the token.

### 6. Telegram commands (modified, +2)

```
/set_token <token>            → overwrite bot_token; chat_id unchanged; reboot
/factory_reset_telegram       → erase /device_config.json; reboot
```

Both gated by the existing `TELEGRAM_CHAT_ID` allowlist that's already enforced in command dispatch — only the operator's chat can send commands, no extra confirm step. Both excluded from `/menu` inline keyboard (one wrong tap shouldn't repoint your device). Listed in `/help` under a new `<b>Identity (DESTRUCTIVE)</b>` section so the danger is visible.

### 7. `/status` additions

Two new fields at the top of the dump:
```
identity=watering bot 2 palm
bot_token=********...iLbJU
```
Label sourced from the matching `DeviceTokenEntry` if MAC was bootstrapped, else `"manual"` for `set_by="api"|"telegram"`, else `"fallback"` for the legacy macro path.

## Data flow

### First boot, known MAC
1. `setup()` → LittleFS mounts → `DeviceToken::init()` runs
2. `/device_config.json` missing → look up `WiFi.macAddress()` in `DEVICE_TOKENS[]`
3. Found → write `{bot_token, chat_id, set_by: "bootstrap", set_unix: now}` to LittleFS
4. Cache token + chat_id in `DeviceToken::cachedToken()` / `cachedChatId()`
5. Telegram comes up against the correct bot. Boot banner sent.

### Second-onward boot
1. `DeviceToken::init()` reads `/device_config.json` directly. No MAC lookup needed.
2. Bot continues against the persisted token regardless of what's in `secret.h`'s table.

### Adding device #3 (token rotation analogue)
1. User edits `secret.h`: append a third `DEVICE_TOKENS[]` entry.
2. Flash device #3 only (devices 1 + 2 already have their tokens in LittleFS, untouched).
3. Device #3 boots, `DeviceToken::init()` bootstraps from the new entry.

### Telegram token swap (live device)
1. User sends `/set_token <new>` to current bot.
2. Bot replies "Token updated. Rebooting." (last message on old bot)
3. `DeviceToken::overwrite()` → `/device_config.json` rewritten → `ESP.restart()`.
4. ~3s later: new boot banner on the **new** bot.

### Factory reset
1. User sends `/factory_reset_telegram` (or `POST /api/device_config/reset`).
2. `DeviceToken::reset()` deletes `/device_config.json` → `ESP.restart()`.
3. On reboot, the resolver re-bootstraps from `secret.h`'s MAC table (or falls back to the macro).

## Error handling

- **Invalid token format (POST):** 400, no write, no reboot. Existing config preserved.
- **MAC not in table AND no persisted config AND macro is empty:** Telegram disabled silently. Serial banner: `[device-token] MAC AA:BB:... unrecognized — Telegram disabled.` `/status` shows `identity=unknown` and `bot_token=disabled`. Web UI and watering loop still work. Manual provisioning via `POST /api/device_config` reactivates it.
- **LittleFS write failure on overwrite:** roll back to old config, return 500. No reboot. Caller can retry.
- **LittleFS read failure on init (corrupt JSON):** treat as missing — re-bootstrap from table. Serial logs the parse error.

## Testing

### Native unit tests (`test/test_unit/test_device_token.cpp`, new)

ArduinoFake's WiFi stub lets us inject a MAC. Cover:
1. `init_with_known_mac_no_persisted_file_bootstraps_from_table`
2. `init_with_persisted_file_ignores_table` (persisted token "wins" even when MAC matches a different table entry)
3. `init_with_unknown_mac_and_empty_macro_returns_isConfigured_false`
4. `init_with_unknown_mac_and_legacy_macro_uses_macro`
5. `overwrite_writes_atomic_temp_then_rename` (verify temp file vanishes)
6. `overwrite_with_invalid_token_returns_false` (validation: <30 chars or no colon)
7. `reset_deletes_file_and_re_init_bootstraps`
8. `overwrite_preserves_chat_id_when_not_provided` (sticky chat_id for the `/set_token` Telegram path which only changes the token)

### Sim tests (`test/sim/test_sim_token_path/`, new)

Use esp32-pio-emulator to verify the full `setup()` happy path: pin a MAC via the simulator's WiFi fake, run setup, assert the right token ends up in the Telegram outgoing URL (visible via `Sim::Network::http_requests()`).

### Manual verification on hardware

After flashing:
1. New device boots → `/status` shows `identity=<label>` matching the secret.h table entry, `set_by=bootstrap`.
2. `/factory_reset_telegram` → ~3s reboot → `set_by=bootstrap` again (re-bootstrap path).
3. `/set_token <other-bot-token>` → boot banner appears on the other bot. `/status` shows `set_by=telegram`.
4. `POST /api/device_config/reset` from another device on the LAN → ~3s reboot → back to bootstrap.

## Migration impact

- Existing v1.0.8 devices in the field: drop in v1.1.0, first boot bootstraps `/device_config.json` from their MAC. Zero manual steps. The legacy `TELEGRAM_BOT_TOKEN` macro stays compiled into the binary as a fallback for boards that aren't in the table yet — same value as v1.0.8 had. Telegram behavior identical until a `/set_token` or factory reset happens.
- No web-UI changes required. The optional `data/web/prod/` could later grow a "Bot config" panel to read/write `/api/device_config`, but it's a separate UX work item.

## Out of scope (deferred)

- WiFi SSID/password runtime config — would expand the surface significantly (boot AP mode for first-time provisioning). Stays compile-time for now.
- OTA_PASSWORD per-device. Same secret today across the fleet. Cheap to add later if needed (same pattern).
- Multi-tenant Telegram chat_ids (each device replies to a different operator). Not a current need; the user is the sole operator.
- A web "Bot config" UI panel. Deferred — `POST /api/device_config` via curl is enough for now.

## Implementation order

1. `include/DeviceToken.h` + native tests (no firmware integration yet).
2. Switch `TelegramNotifier.h` from macros to `DeviceToken::token()` / `chatId()`. Run native tests + flash one device, verify behavior unchanged with the existing token.
3. Wire `DeviceToken::init()` into `setup()` (after LittleFS mount, before `NetworkManager::connectWiFi()`).
4. Populate `secret.h`'s `DEVICE_TOKENS[]` table with both devices' MACs.
5. Reflash both devices. Verify each gets the right bot from its MAC (bootstrap path).
6. Add `/api/device_config*` endpoints.
7. Add `/set_token` and `/factory_reset_telegram` Telegram commands.
8. Update `/status`, `/help`, bot commands JSON.
9. Bump to v1.1.0 (minor — semantic change to identity model). Commit + push.

Estimated effort: ~110 LOC firmware + ~80 LOC tests + ~30 LOC docs. Single-day implementation.
