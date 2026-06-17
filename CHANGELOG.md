# Changelog

## v1.0.1 — Bugfix

### Fixed
- **GameSwitcher now registers games launched from the app.** Launching through
  the app bypasses MainUI, which is what normally writes the OnionOS recent list
  that GameSwitcher reads — so launched games got no GameSwitcher card or resume
  screenshot (only play‑activity/playtime was recorded). The app now writes the
  game's `type:5` recent entry itself (in MainUI's exact rom‑path form, so it
  dedupes against native launches and reuses the same screenshot) and merges it
  into the active `recentlist*.json` atomically. The merge is strictly additive:
  if it ever fails, the game still launches as before.

## v1.0 — Initial release

The first public release of **Mini Tracker** — a backlog & completion tracker for OnionOS on the
Miyoo Mini family.

### Highlights
- **Status tracking** — Backlog / Playing / Beaten / Abandoned for every game in your library.
- **S–F tier ranking** for beaten or abandoned games, with a full **Tier List** (overall or
  per‑console) and a scrollable **drill‑in** to browse any tier.
- **Stats** — completion %, library mix, beaten‑by‑month chart, and a most‑played leaderboard.
- **Now Playing** strip on the home screen, with native game launching (shows up in OnionOS
  recents / GameSwitcher).
- **Celebrations** — animated **BEATEN!** (confetti) and **DROPPED** (rain) moments, each leading
  into a rank prompt.
- **“Roll the dice”** random backlog picker.
- **Box‑art grid + list views**, **5 themes** (incl. a light theme), colorblind‑friendly status icons.
- **Built‑in branded screenshots** (saved as PNG to `/mnt/SDCARD/Screenshots`).
- **First‑run onboarding** carousel with a live theme picker and an opt‑in favorites‑sync step.
- Reads **`miyoogamelist.xml`** for curated names + art; **opt‑in favorites sync** with OnionOS.

### Data & safety
- Reads the OnionOS play‑activity database **read‑only**; owns a separate `tracker.sqlite`.
- Writes `favourite.json` only when favorites sync is enabled (backs it up first).
- Prunes games whose ROM has been deleted (guarded so a half‑mounted card can’t wipe the library).
- No network, no telemetry, no firmware changes. Uninstall = delete the app folder.

### Engineering
- Fully‑static **musl ARMv7‑A** binary; SQLite, stb_image, and JetBrains Mono compiled in.
- Pre‑release audit hardening: S‑tier rating persistence, console restore on crash/SIGTERM,
  input/framebuffer‑bpp guards, JSON/shell escaping for names with special characters, and the
  non‑game extension filter applied to the playtime import.

### Known limitations
- The 2024 Miyoo Mini (752×560) runs but the layout isn’t optimized for its larger screen yet.
- Some standalone PORTS / `.sh` “games” track and rank fine but may not launch from the app.
- Rumble defaults on (safe no‑op on devices without a motor).
