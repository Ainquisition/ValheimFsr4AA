# Corresponding OptiScaler source

The release contains an unmodified local build of OptiScaler, GPL-3.0.
Exact corresponding upstream source: https://github.com/optiscaler/OptiScaler/tree/0e42d5955405360157c22154b4a13fdf3fc196a6
Source archive: https://github.com/optiscaler/OptiScaler/archive/0e42d5955405360157c22154b4a13fdf3fc196a6.tar.gz

The archive above does not embed submodule contents. Obtain the complete source with its exact dependencies:

```powershell
git clone --no-checkout https://github.com/optiscaler/OptiScaler.git OptiScaler
git -C OptiScaler checkout --detach 0e42d5955405360157c22154b4a13fdf3fc196a6
git -C OptiScaler submodule update --init --recursive
```

The pinned tree and all recursive submodule revisions used for the included DLL are listed below. Build instructions are in build-optiscaler.ps1; the main project was not modified. Source is served by the original upstream GitHub repositories separately from the binaries. The same download instructions are included inside the binary archive. No proprietary game/BepInEx DLLs are redistributed.

```text
 c6efa6bf7f2027b3ec94f28578bb5965eabb9e55 external/FidelityFX-SDK (c6efa6b)
 60f4ea81909200d8542eca14dccb2628b763a9a3 external/FidelityFX-SDK-v2 (v2.3.0)
 a733a2ea665ca5d72b7270f0334bf2e7b82bd0cc external/magic_enum (a733a2e)
 9b181ea572f680327fe01a14a0f1f41c78034104 external/nvapi (9b181ea)
 6048871ea9ee0ec24be5bd099d161a10567d7dc2 external/simpleini (6048871)
 faa0a7a9c5a3550ed5461fab7d8e31c37fd1a2ef external/spdlog (faa0a7a)
 73f3cbb237e84d483afafc743f1f14ec53e12314 external/unordered_dense (73f3cbb)
 d64e9e156ac818c19b722ca142230b68e3daafe3 external/vulkan (d64e9e1)
 8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0 external/xess (8fe81bd)
```
