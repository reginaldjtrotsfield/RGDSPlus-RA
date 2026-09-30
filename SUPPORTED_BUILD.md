# Supported Emulator Build

RGDSPlus-RA v0.1.0 uses runtime hooks at known offsets and must only be used with the exact NNDDSS / libnnddss build it was tested against.

```text
336caf2fd154e5ab2ceefe0eb60cefe8b28d4fbdf4790c834b83eaa9b38b93cd  /mnt/vendor/deep/nnddss/nnddss.real
5d58c35bbf269fa4dedbac76f99d1614c2f55b2e5e1dd3a08eaecf9737453fe3  /mnt/vendor/deep/nnddss/lib/libnnddss.so
```

The installer must refuse to install if these hashes do not match.
