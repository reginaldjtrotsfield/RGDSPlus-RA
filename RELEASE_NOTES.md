# RGDSPlus-RA v0.1.1

Second public preview release of RetroAchievements support for the stock RG DS Plus NNDDSS/DraStic-based Nintendo DS emulator.

v0.1.1 introduces a simplified SD-card installation flow using the built-in Applications menu.

## Installation

- Extract the release package directly into the SD card `Roms/APPS` folder.
- Run `RGDSPlus-RA-Install.sh` from Applications → Apps.
- The installer returns to the Applications menu without requiring a reboot.
- Launch Nintendo DS games normally.

An uninstall script is included to restore the stock emulator.

## Account persistence

- RetroAchievements login credentials are preserved across reinstallations.
- Users only need to sign in once.

## Hardcore status

Hardcore Mode is implemented locally and the required restrictions are enforced.

RGDSPlus-RA is not yet approved for official RetroAchievements Hardcore credit. Until approval, enabling Hardcore will produce the expected **Unknown Emulator** warning and achievements will be credited as Softcore.

## Package contents

No NNDDSS, DraStic binaries, firmware, BIOS files, ROMs, or RetroAchievements credentials are included.

Client identity:

```text
RGDSPlus-RA/0.1.1 rcheevos/12.5

```
