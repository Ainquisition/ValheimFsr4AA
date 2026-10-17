import importlib.util
import json
from pathlib import Path
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root))
spec = importlib.util.spec_from_file_location("stack",root / "switch-stack.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
work = root.parents[1] / "work"
with tempfile.TemporaryDirectory(prefix="stack-rollback-check-",dir=work) as folder:
    scratch = Path(folder).resolve()
    assert scratch.is_relative_to(work.resolve())
    game,profile = scratch / "game",scratch / "profile"
    native = game / "valheim_Data/Plugins/x86_64/ValheimFsr4AA.Native.dll"
    config = profile / "BepInEx/config/com.wubarrk.graphicsoverdrive.cfg"
    module.write(native,(root / "bin/ValheimFsr4AA.Native.dll").read_bytes())
    module.write(game / "valheim.exe",b"fixture, never executed")
    module.write(game / "valheim_Data/globalgamemanagers",b"preload fixture")
    original = b"[Master]\r\nMasterEnable = true\r\nExclusiveControl = true\r\n[Native Scaler]\r\nScaler = FSR3\r\nOther = keep\r\n"
    module.write(config,original)
    module.write(game / "dxgi.dll",b"test-owned fake proxy")
    module.OLD_PROXY = module.file_hash(game / "dxgi.dll")
    previous = {"state":"installed","files":[{"after":module.file_hash(game / "valheim_Data/globalgamemanagers")},{},{"after":module.file_hash(native)}]}
    module.write(game / "ValheimFsr4AA-backup-0.1.1/manifest.json",json.dumps(previous).encode())
    module.switch("install",game,profile)
    assert not (game / "dxgi.dll").exists()
    assert (game / "dxgi.dll.before-vf4").read_bytes() == b"test-owned fake proxy"
    assert config.read_bytes() == original.replace(b"ExclusiveControl = true",b"ExclusiveControl = false").replace(b"Scaler = FSR3",b"Scaler = Vanilla")
    upgrade_spec = importlib.util.spec_from_file_location("upgrade",root / "upgrade-adapter.py")
    updater = importlib.util.module_from_spec(upgrade_spec)
    upgrade_spec.loader.exec_module(updater)
    active = game / "ValheimFsr4AA-backup-0.2.0/manifest.json"
    state = json.loads(active.read_text())
    for index, entry in enumerate(state["files"][:2]):
        target = Path(entry["path"])
        target.write_bytes(b"previous own DLL fixture " + str(index).encode())
        entry["after"] = module.file_hash(target)
    active.write_text(json.dumps(state))
    updater.upgrade(game,profile)
    updater.upgrade(game,profile)
    assert (game / "ValheimFsr4AA-backup-0.2.2/0.bin").read_bytes() == b"previous own DLL fixture 0"
    assert native.read_bytes() == (root / "bin-adapter/ValheimFsr4AA.Native.dll").read_bytes()
    # Exercise safe uninstall after the runtime UI modifies its own ini.
    ini = game / "ValheimFsr4AA-runtime/OptiScaler.ini"
    ini.write_bytes(ini.read_bytes() + b"\n; synthetic UI edit\n")
    config.write_bytes(config.read_bytes() + b"\r\n; synthetic later config edit\r\n")
    module.switch("restore",game,profile)
    module.switch("restore",game,profile)
    assert (game / "dxgi.dll").read_bytes() == b"test-owned fake proxy"
    assert not (game / "dxgi.dll.before-vf4").exists()
    assert config.read_bytes() == original
    assert native.read_bytes() == (root / "bin/ValheimFsr4AA.Native.dll").read_bytes()
    assert list((game / "ValheimFsr4AA-stack-backup-0.2.0").glob("OptiScaler-after-use-*.ini"))
    assert list((game / "ValheimFsr4AA-stack-backup-0.2.0").glob("GraphicsOverdrive-after-use-*.cfg"))
print("PASS full stack backup/install/restore, exact DLL/config bytes, retained UI edits, repeated restore")
