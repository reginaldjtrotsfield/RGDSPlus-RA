# Hardcore Status

RGDSPlus-RA is currently **not an approved RetroAchievements Hardcore emulator/client**.

The public v0.1.0 preview therefore enables rcheevos spectator mode and does not submit achievements or leaderboard scores.

Implemented protections include:
- three guarded load-state execution sites
- early load-state guard installation before normal startup resumes
- runtime cheat-enable blocking
- persisted cheat suppression before DraStic builds its active cheat list
- save-state creation remains allowed
- fast-forward remains allowed

No rewind, slowdown, or frame-advance functionality was found during the audit.

A controlled development test successfully submitted a real achievement, but the server correctly recorded it as Softcore because the client is not approved. Public preview builds remain in spectator mode while approval is pending.
