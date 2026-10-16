# Install and build

## Install

Download the compiled ZIP linked in README and extract it into a separate folder. It includes the adapter, OptiScaler, AMD/DX12 components, configuration, source and licenses. Install BepInEx separately. Close Valheim before changing files.

The scripts target Valheim 1.0.20 / Unity 6000.0.75f1. They verify file hashes and preserve backups. From the extracted folder:

```powershell
python deploy.py check
python deploy.py install
python deploy-adapter.py check
python deploy-adapter.py install
```

Defaults target `D:\SteamLibrary\steamapps\common\Valheim` and the Thunderstore Default profile. Override them with `--game` and `--profile`.

If the checked legacy OptiScaler 0.9.4 / GraphicsOverdrive stack conflicts, use `python switch-stack.py install` instead of the final command. It backs up the known proxy DLL and settings, renames that proxy, and sets `ExclusiveControl=false`, `Scaler=Vanilla`. Other proxy versions are refused.

Start `Launch-adapter.cmd`, load a world at 2560×1440 and press F9. Select FSR in OptiScaler. Check the FSR4 Native-AA 1.00× watermark; a successful NGX call alone does not identify the active backend. Ordinary mod-manager launch does not set the required early-loading environment variable.

Update an installed 0.2.0 with `python upgrade-adapter.py`.

## Remove

For a switched stack, run `Restore-previous.cmd`. For a direct adapter install, run `python deploy-adapter.py remove`. Then `python deploy.py remove` removes the diagnostic/preload stage. Keep backups until restoration is checked.

## Build

Requires PowerShell, Git, Python 3, .NET SDK, MSVC x64 Build Tools and Windows SDK. Tested with .NET SDK 10.0.103 and MSVC 19.51. Set local paths, then run from the repository root:

```powershell
$deps = Join-Path $PWD 'work'
$game = 'D:\SteamLibrary\steamapps\common\Valheim'
$core = "$env:APPDATA/Thunderstore Mod Manager/DataFolder/Valheim/profiles/Default/BepInEx/core"
.\build.ps1 -GamePath $game -BepInExCore $core -DependencyRoot $deps
.\build-optiscaler.ps1 -DependencyRoot $deps
.\build-adapter.ps1 -Game $game -Core $core -DependencyRoot $deps
.\stage-runtime.ps1 -DependencyRoot $deps
```

Commits and dependency gitlinks are pinned in `sources.lock.json`. Game and BepInEx DLLs are local references, excluded from the package.

## Verify

```powershell
python src/test_adapter_deploy.py
python src/test_switch_stack.py
.\checks/run.ps1 -NgxContext -RuntimePackage runtime-candidate -DependencyRoot work
```

Close the game before the GPU test. It uses an isolated local scene and test-only CPU readback. The adapter does not read textures back each frame. `test_preload.py` needs an original matching `globalgamemanagers`; the historical `test_deploy.py` needs a saved diagnostic 0.1.0 game/profile fixture and a scratch directory. All four Python checks passed before publication.

Version 0.2.1 fixes alternating FSR/TAA frames, repeated history resets and the inverted watermark. Fixture tests verify RGB orientation, frame-slot ownership and processing without history resets. World, camera, teleport and alt-tab checks were reported by the owner; separate timings for those scenarios were not recorded.

Outside 2560×1440 the adapter falls back to ordinary TAA. GPU fence waits remain synchronous. Output alpha repeats the red channel in the fixture; RGB is verified. Long stress tests and other mod combinations remain untested.
