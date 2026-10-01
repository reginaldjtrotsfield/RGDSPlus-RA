# Hardcore Status

RGDSPlus-RA implements RetroAchievements Hardcore Mode locally, but it is **not yet an approved RetroAchievements Hardcore emulator/client**.

## v0.1.1 behavior

v0.1.1 disables spectator mode and enables normal achievement submissions.

Hardcore Mode can be enabled from the RetroAchievements menu and the frontend's Hardcore restrictions are enforced locally.

Until RGDSPlus-RA is added to RetroAchievements' accepted emulator list, starting a game with Hardcore enabled will produce the expected:

```text
Warning: Unknown Emulator
```

Achievements earned while Hardcore is enabled locally are currently credited as **Softcore** by RetroAchievements.

This behavior has been tested and is expected prior to official Hardcore approval.

## Implemented Hardcore protections

RGDSPlus-RA currently includes:

- guarded load-state execution paths
- early load-state guard installation before normal startup resumes
- runtime cheat-enable blocking
- persisted cheat suppression before DraStic builds its active cheat list
- save-state creation remains allowed
- fast-forward remains allowed

No rewind, slowdown, or frame-advance functionality was found during the frontend audit.

The Hardcore implementation has been tested against:

- load-state attempts while Hardcore is active
- attempts to enable cheats while Hardcore is active
- cheats enabled in Casual Mode before restarting into Hardcore
- startup with Hardcore already enabled

## RetroAchievements approval

The current implementation is intended to function as official Hardcore once RGDSPlus-RA completes RetroAchievements verification and is added to the accepted emulator list.

Until then, the local Hardcore restrictions remain active, but the server records unlocks as Softcore.
