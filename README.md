# RGDSPlus-RA

RetroAchievements integration for the stock Nintendo DS emulator included with the Anbernic RG DS Plus.

RGDSPlus-RA does **not** replace the Nintendo DS emulator. It is an `LD_PRELOAD` runtime integration layer around the existing NNDDSS / DraStic-based frontend.

## Status: Casual Mode only

RGDSPlus-RA is currently intended for RetroAchievements **Casual Mode only**.

Experimental Hardcore support has been implemented, but it is **not approved by RetroAchievements** and should not be used for official Hardcore achievements or leaderboards.

The initial v0.1.0 public preview runs in spectator mode, so it evaluates achievements locally without submitting unlocks or leaderboard scores. A Casual-only release with submissions enabled is being prepared.

## Features

- RetroAchievements login
- Nintendo DS game identification and hashing
- Achievement evaluation using rcheevos 12.5
- Achievement menu
- Badges and popup notifications
- Achievement unlock sound
- Rich Presence
- Challenge Indicators
- Progress Indicators
- Asynchronous RetroAchievements networking
- Persistent RetroAchievements settings

Experimental Hardcore-related development is documented separately in `docs/HARDCORE_STATUS.md`.

## Hardcore protections

When Hardcore is enabled locally:

- all three known NNDDSS load-state execution paths are guarded
- load-state guards are installed before normal emulator startup resumes
- runtime cheat enabling is blocked
- cheats previously enabled in Casual are suppressed before DraStic builds its active cheat list
- save-state creation remains allowed
- fast-forward remains allowed

No rewind, slowdown, or frame-advance functionality was found in this frontend during development.

## Client identity

```text
RGDSPlus-RA/0.1.0 rcheevos/12.5
```

## Compatibility

This release uses validated runtime offsets and currently supports only the exact emulator build listed in `SUPPORTED_BUILD.md`.

## What this repository does not contain

This project does not distribute NNDDSS, DraStic binaries, Anbernic firmware, Nintendo DS BIOS/firmware files, ROMs, or RetroAchievements credentials.

## Logs

`/tmp/ra_frontend.log`

## License

RGDSPlus-RA integration code is released under the MIT License. Third-party components retain their original licenses. See `THIRD_PARTY_NOTICES.md`.
