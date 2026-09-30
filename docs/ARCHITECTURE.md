# Architecture

RGDSPlus-RA is not a Nintendo DS emulator.

```text
Anbernic launcher
       |
       v
RGDSPlus-RA wrapper
       |
       +-- RA live/rcheevos preload
       +-- RA popup preload
       +-- RA settings/menu preload
       |
       v
NNDDSS frontend
       |
       v
DraStic-based libnnddss.so core
```

The integration uses runtime symbol interception and validated AArch64 call-site patches to connect rcheevos to emulated Nintendo DS memory and enforce Hardcore restrictions.

The proprietary NNDDSS/DraStic binaries are not modified on disk and are not distributed by this project.
