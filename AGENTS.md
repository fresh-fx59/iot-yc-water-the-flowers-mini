# AGENTS.md

Project-level rules for any AI agent (Claude Code, Copilot, Gemini CLI, etc.) working in this repo. These apply IN ADDITION to `CLAUDE.md` (architecture / build / commands) — read both.

## Hard rules (do not violate)

### 1. Never expose secrets

`include/secret.h` is gitignored and contains live credentials: WiFi password, OTA basic-auth password, multiple Telegram bot tokens (`TELEGRAM_BOT_TOKEN`, plus per-device entries in `DEVICE_TOKENS[]`), bearer tokens for the Cloud.ru proxy, and any other tokens you wire in.

- **Never `cat`, `Read`, or otherwise echo `secret.h` contents into chat output, log files, planning docs, or anything the user might paste elsewhere.** If you need to edit `secret.h`, target the specific line via `Edit` without printing surrounding lines back.
- **Never commit any file containing a token.** This includes `*.bin` and `*.elf` build outputs — compiled firmware embeds the bot token as a string constant. `.gitignore` already covers `*.bin` and `include/secret.h`; do not undo that, and do not `git add -A` or `git add .` without verifying nothing sensitive sneaks in.
- If a token is accidentally exposed (chat, repo, log), tell the user immediately so they can rotate it. Don't try to hide it.

### 2. Use the test bot for hardware tests, not production

`DEVICE_TOKENS[]` in `secret.h` maps each device's WiFi MAC to its bot token + label. There is a dedicated **test bot entry** (label `"test watering bot"`) for hardware-in-the-loop validation. When working with a test ESP32:

- Add the test ESP32's MAC to the test-bot row in `DEVICE_TOKENS[]` so `DeviceToken::init()` resolves to the test bot.
- Watch serial output for `[device-token] MAC <addr> not in DEVICE_TOKENS[] — falling back` and `[device-token] using compile-time macro fallback (...)` — both indicate the device is about to use the production `TELEGRAM_BOT_TOKEN` macro. **Stop and fix the MAC mapping before triggering any test action** (OTA, watering, anything that emits Telegram).
- Production bots (`@iot_alex_watering_1_bot`, `@iot_alex_watering_2_bot`) are tied to a live chat the user reads daily. Test traffic must not land there.

### 3. OTA hardware testing rig

The pull-OTA flow (`include/FirmwareUpdater.h`) is verified end-to-end against a local mock server, not Cloud.ru — `tools/ota_test_server.py` plus the `[env:esp32-s3-ota-test]` PlatformIO env in `platformio.ini` redirect `METRICS_PROXY_BASE_URL` to `http://<laptop-LAN-IP>:8765`. The HTTP API endpoints `POST /api/check_update` and `POST /api/rollback` (in `include/api_handlers.h`) drive the tests via curl without needing Telegram — but the device will still emit progress notifications through its configured bot, so rule #2 still applies.

Trigger `/check_update` only after confirming the device resolved to the test bot. The bot token shown in the serial preview (`********<5-char-suffix>`) should match the test bot's, not bot 1 or bot 2.

### 4. Don't push to remotes unprompted

Never `git push` without the user explicitly asking. Local commits to private branches are fine when authorised; pushing to `origin` (a public GitHub remote on this project) requires an explicit instruction from the user for each push.
