# Mini Tracker — Feature Tour

A walkthrough of everything Mini Tracker does. Doubles as a script for a demo video — each section
is roughly one "scene."

---

## 1. Home screen

The hub. Three zones, navigated with the D‑pad:

- **Now Playing strip** (top) — box art of every game you've marked *Playing*, sorted by playtime.
  Press **Up** from the list to hop into it, scroll with **L/R**, and **A** opens the game (where you
  can launch it). It scrolls through *all* in‑progress games, not just the first few.
- **Stats & Tier List banner** — a tappable card showing a live summary (beaten count, %, in‑progress).
  **A** opens the Stats screens.
- **Console list** — *All Games* plus one row per system, each with a completion bar and %.
  **A** opens it; **X** rolls a random pick; **Y** opens Settings.

## 2. Opening a game — the detail page

Press **A** on any game (in a list, the grid, the Now Playing strip, or a tier) to open its page:

- **Left:** box art, console, hours played, last played, and (for beaten games) the date + tier badge.
- **Right:** the **current status** is shown as a colored pill in the title bar *and* marked in the
  status list, so it's always obvious. Pick **Backlog / Playing / Beaten / Abandoned**, **Change rating**,
  **Launch game**, or **Hide** (for rogue/no‑art junk entries — hidden games are excluded from all stats).

## 3. Status & the celebration moments

Set a game's status with one button. Two of them get a moment:

- **Beaten →** a confetti **BEATEN!** celebration with a counting‑up stat and your current completion,
  then a **rank prompt**.
- **Abandoned →** a quieter, rainy **DROPPED** animation ("Not every game is for you"), then a rank prompt.

## 4. Tier ranking (S–F)

Beaten or abandoned games can be ranked **S, A, B, C, D, F** — classic tier‑list colors. Pick the rank
from a big icon chooser. Ranks show up as a colored letter badge on covers and rows, and you can sort
any list by tier (**X** to cycle sorts).

## 5. Stats — Overview & Tier List

Open from the home banner. **L/R** switches scope between **All Games** and each console; **Y** toggles tabs:

- **Overview** — a big completion bar, a stacked **library mix** (backlog/playing/beaten/dropped),
  a **beaten‑by‑month** chart, and a **most‑played** leaderboard.
- **Tier List** — your ranked games laid out in S→F rows. **Up/Down** selects a tier and **A** drills
  into a **full, scrollable grid** of just that tier (so it scales past what fits on one row). Previews
  are ordered most‑played first.

Press **X** on either tab to **save a branded screenshot** (PNG) to the SD card — great for sharing a tier list.

## 6. Roll the dice

Can't decide what to play? **X** on the home screen pulls a **random backlog game**. Launch it, mark it
Playing, or **X** to reroll.

## 7. Views, themes & settings

- **Grid or list** — **Y** toggles in any list; set a default in Settings. On first run the app auto‑picks
  grid (if you have box art) or list (if you don't).
- **5 themes** — Teal Dusk (default), Sunset, Matrix, Ocean, and a **Lite** light theme. Pick in Settings
  or during onboarding (live preview).
- **Settings** — favorites sync, rumble strength, default sort, default view, show‑hidden, hero backdrop,
  theme, plus Help and About.

## 8. Favorites sync (opt‑in)

Enable it and your **Playing** games mirror into the OnionOS **Favorites** folder — a quick‑launch
shortcut from the system home screen. It's off by default, asks for confirmation, backs up `favourite.json`
first, and never changes your Beaten/Abandoned marks.

## 9. First‑run onboarding

New users get a short, skippable carousel: welcome → how it works → navigation → **theme picker (live)** →
**favorites sync (opt‑in)** → done. It runs once.

---

### Under the hood

- Reads your `/Roms` library and OnionOS's play‑activity DB (**read‑only**), and uses each system's
  `miyoogamelist.xml` for curated names and art when present.
- Launches games through OnionOS's own pipeline, so they register in recents / GameSwitcher.
- Everything you set lives in a separate `tracker.sqlite` — OnionOS data is never modified.
