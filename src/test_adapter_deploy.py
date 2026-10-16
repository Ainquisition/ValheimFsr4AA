"""Scratch-only transaction check; never uses the installed game as a write target."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root))
spec = importlib.util.spec_from_file_location("candidate_deploy", root / "deploy-adapter.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
scratch_root = root.parents[1] / "work"
with tempfile.TemporaryDirectory(prefix="adapter-deploy-check-", dir=scratch_root) as folder:
    scratch = Path(folder).resolve()
    game, profile = scratch / "game", scratch / "profile"
    native = game / "valheim_Data/Plugins/x86_64/ValheimFsr4AA.Native.dll"
    module.write(native, (root / "bin/ValheimFsr4AA.Native.dll").read_bytes())
    module.write(game / "valheim.exe", b"fixture, not executed")
    module.write(game / "valheim_Data/globalgamemanagers", b"fixture preload asset")
    module.write(game / "unrelated.dll", b"foreign fixture, preserve")
    module.write(game / "dxgi.dll", b"foreign proxy fixture, preserve")
    previous = {"state":"installed", "files":[
        {"after":module.file_hash(game / "valheim_Data/globalgamemanagers")}, {}, {"after":module.file_hash(native)}]}
    module.write(game / "ValheimFsr4AA-backup-0.1.1/manifest.json", json.dumps(previous).encode())
    snapshot = {str(p.relative_to(scratch)):p.read_bytes() for p in scratch.rglob("*") if p.is_file()}
    module.deploy("check", game, profile)
    try:
        module.deploy("install", game, profile)
        raise AssertionError("Foreign proxy was not refused")
    except ValueError:
        pass
    assert snapshot == {str(p.relative_to(scratch)):p.read_bytes() for p in scratch.rglob("*") if p.is_file()}
    # Remove only the synthetic file this test just created, under its verified scratch directory.
    assert (game / "dxgi.dll").resolve().is_relative_to(scratch)
    (game / "dxgi.dll").unlink()
    module.deploy("install", game, profile)
    module.deploy("install", game, profile)
    managed = profile / "BepInEx/plugins/ValheimFsr4AA/ValheimFsr4AA.Adapter.dll"
    original = managed.read_bytes()
    managed.write_bytes(b"independent edit")
    native_after = native.read_bytes()
    try:
        module.deploy("remove", game, profile)
        raise AssertionError("Independent change was not refused")
    except ValueError:
        pass
    assert native.read_bytes() == native_after and managed.read_bytes() == b"independent edit"
    managed.write_bytes(original)
    module.deploy("remove", game, profile)
    assert native.read_bytes() == (root / "bin/ValheimFsr4AA.Native.dll").read_bytes()
    assert not managed.exists()
    assert not list((game / "ValheimFsr4AA-runtime").rglob("*.dll"))
    assert (game / "unrelated.dll").read_bytes() == b"foreign fixture, preserve"
print("PASS: foreign conflict refused without writes, install, idempotence, changed-file refusal, exact native rollback")
