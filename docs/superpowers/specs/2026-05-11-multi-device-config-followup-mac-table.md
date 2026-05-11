# Multi-device runtime config — follow-up: bot-token-per-MAC

**Context:** After reading the prior doc (`2026-05-11-multi-device-config-research.md`), the user clarified:
> *chat_id is the same for both devices, because the operator's Telegram account is the same.*

That changes the analysis. Only `TELEGRAM_BOT_TOKEN` differs per device — `TELEGRAM_CHAT_ID` is shared. With the variable surface shrunk to one string, the simplest viable approach becomes more attractive than the original recommendation.

## Revised recommendation

**Option C (MAC-keyed lookup table in `secret.h`)** now wins for the user's fleet size (2 devices today, plausibly 3–5 long-term). Smaller scope than Option B (runtime config), no provisioning UX needed.

The original doc rated Option C lower because the chat_id was also assumed per-device, doubling the table width. With chat_id shared, the table is one column of bot tokens keyed by MAC.

## How it works

Every ESP32-S3 has a unique factory-burned MAC address, readable at runtime via `WiFi.macAddress()` (returns `"AA:BB:CC:DD:EE:FF"` style string) or `ESP.getEfuseMac()` (returns `uint64_t`). The MAC is set in eFuse — survives reboots, reflashes, brick-and-recovery — so it's a stable per-device identifier with zero per-device wiring or config.

Each device's MAC is already visible:
- During boot, the Arduino-ESP32 logs `[WiFi] Hostname: esp32s3-xxxxxx` with the last 6 hex digits.
- Or call `Serial.println(WiFi.macAddress())` in `setup()`.
- Or run `pio device monitor` after `setup()`, where `WL_CONNECTED` is logged with the local IP — the router's DHCP lease table also shows the MAC.

Once the user knows each device's MAC, the firmware contains a hard-coded lookup table:

```cpp
// secret.h additions (replaces the single TELEGRAM_BOT_TOKEN macro)
struct DeviceTokenEntry {
    const char* mac;        // 17-char upper-case "AA:BB:CC:DD:EE:FF"
    const char* bot_token;  // Telegram bot token
};

static const DeviceTokenEntry DEVICE_TOKENS[] = {
    { "AA:BB:CC:DD:EE:01", "1111111111:AAAAAA..." },
    { "AA:BB:CC:DD:EE:02", "2222222222:BBBBBB..." },
};
static const size_t DEVICE_TOKEN_COUNT =
    sizeof(DEVICE_TOKENS) / sizeof(DEVICE_TOKENS[0]);

// Optional fallback for an unrecognized MAC (development boards, swaps).
// Empty string => Telegram disabled on this device until secret.h is updated.
#define TELEGRAM_BOT_TOKEN_FALLBACK ""

// TELEGRAM_CHAT_ID stays as-is — same operator account.
#define TELEGRAM_CHAT_ID "314102923"
```

At boot, the firmware resolves its own MAC to a token once and caches it:

```cpp
// New: include/DeviceToken.h
#ifndef DEVICE_TOKEN_H
#define DEVICE_TOKEN_H

#include <Arduino.h>
#include <WiFi.h>
#include <secret.h>

namespace DeviceToken {
inline String& cached() {
    static String t;
    return t;
}

inline const char* resolve() {
    String& t = cached();
    if (t.length() == 0) {
        String mac = WiFi.macAddress();      // upper-case AA:BB:CC:DD:EE:FF
        for (size_t i = 0; i < DEVICE_TOKEN_COUNT; ++i) {
            if (mac.equalsIgnoreCase(DEVICE_TOKENS[i].mac)) {
                t = DEVICE_TOKENS[i].bot_token;
                return t.c_str();
            }
        }
        t = TELEGRAM_BOT_TOKEN_FALLBACK;
        Serial.printf("[device-token] no entry for MAC %s — Telegram disabled\n",
                      mac.c_str());
    }
    return t.c_str();
}
}  // namespace DeviceToken

#endif
```

Then in `TelegramNotifier.h`, every reference to `TELEGRAM_BOT_TOKEN` (12 sites today) becomes `DeviceToken::resolve()`:

```cpp
// before:
String url = String("https://api.telegram.org/bot") + TELEGRAM_BOT_TOKEN + ...

// after:
String url = String("https://api.telegram.org/bot") + DeviceToken::resolve() + ...
```

`DeviceToken::resolve()` returns `const char*` like the macro, so the rest of the codebase is unchanged. The cached `String` keeps storage alive across calls.

`TelegramNotifier::sendMessage()` / `checkForCommands()` / `ensureBotCommandsRegistered()` should also short-circuit when the token is empty (the unrecognized-MAC fallback case) — same guard pattern the original doc proposed for `isConfigured()`.

## Effort estimate

| Change | LOC | Files |
|---|---|---|
| Add `DeviceToken` namespace | ~30 | `include/DeviceToken.h` (new) |
| Migrate `secret.h` | ~10 | `include/secret.h` (per-device, not committed) |
| Replace `TELEGRAM_BOT_TOKEN` references | ~12 lines | `include/TelegramNotifier.h` |
| Empty-token guards | ~5 lines × 3 sites | `include/TelegramNotifier.h` |
| Update `secret.h.template` | ~10 | `include/secret.h.template` (if exists) |
| Native tests | 1 file | `test/test_device_token.cpp` — round-trips MAC→token resolution with an `ArduinoFake` MAC stub |
| **Total** | **~80 LOC** | 3 files modified, 2 new |

Compare to Option B (full LittleFS provisioning) at ~300 LOC + web UI changes — about 4× less work.

## Trade-offs vs. Option B

| Question | Option C (MAC table) | Option B (LittleFS config) |
|---|---|---|
| Per-device flashing | One firmware fits all | One firmware fits all |
| Provisioning step per new device | Append a row to `secret.h`, reflash *every* device once | Flash any device, then `POST /api/device_config` via web UI |
| Fleet size sweet spot | 2–5 devices | 5+ |
| Token rotation (Telegram revokes) | Edit one row, reflash all | `POST /api/device_config` on one device |
| First-boot UX | Boots silently if MAC unknown (Telegram disabled, web UI works) | Boots silently if `/device_config.json` missing (same UX) |
| `secret.h` grows with | Fleet size | Stays small |
| Lines changed | ~80 | ~300 |
| Risk of secret leak | Same — secret.h still has every token | Same — secrets persisted to LittleFS partition |

Option C's main weakness — "adding device #N means reflashing everyone" — is mitigated by the OTA endpoint already in `ota.h`. Curl the new firmware to each device on the network in a one-liner (see `2026-05-11-ota-firmware-littlefs-research.md`). For a 2-device fleet that's a 30-second operation.

## When to switch from C to B later

If any of these become true, migrate:
1. Fleet grows past ~5 devices.
2. Devices live on multiple WiFi networks (different operators), so a single OTA-curl loop can't reach them all.
3. Token rotation becomes frequent (more than monthly).
4. A non-technical operator needs to provision their own device.

Option B has lower per-device-add cost; the upfront cost is one-time. Option C has lower upfront cost; the per-device-add cost is "reflash all." The crossover for a hobbyist fleet is around device #5.

## Open decision for the user

1. **MAC source.** `WiFi.macAddress()` (STA interface — the one Telegram traffic uses) or `ESP.getEfuseMac()` (factory eFuse, raw 6-byte). Both stable. STA MAC is easier to read off the router's DHCP table; eFuse MAC is the raw chip ID. I'd default to **STA MAC** — matches what you see in routers and the printed boot log.
2. **Unknown-MAC behavior.** Silent disable (proposed above) or panic/halt? Silent is safer — a freshly-flashed device on the bench shouldn't refuse to boot. A startup serial line + a `/status` field (`telegram_enabled: false`) makes it observable.
3. **Where the table lives.** `secret.h` (current proposal — gitignored) or a separate `device_tokens.h` (also gitignored). Splitting them is cosmetic; same effective protection.

## Recommendation

**Go with Option C (MAC-keyed table) for the v1.x line.** Smaller change, faster to ship, fits a 2-device fleet cleanly. Revisit Option B when the fleet grows or token rotation becomes operationally painful. Both options are forward-compatible — the v1.0.6 NTP work, v1.0.7 status/menu polish, and OTA `/firmware` endpoint all stay relevant in either path.

If you want me to implement, the next step is:
- Bump to v1.1.0 (semantic change to identity model).
- Add `include/DeviceToken.h` and the `secret.h` table.
- Replace the 12 `TELEGRAM_BOT_TOKEN` references in `include/TelegramNotifier.h`.
- Add `test/test_device_token.cpp` with a stubbed MAC.
- Update `MOTHER_PROJECT.md` and `CLAUDE.md` to document the MAC→token pattern.
