"""Candidate installer. Does not change foreign DLLs or graphics-mod settings."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
from deploy import ROOT, file_hash, write, restore
from preload import digest, require

INSTALLED_NATIVE_011 = "a01cd44abc5b5e4b4a33d901cbc388cf6f5bbe39926188293bc10c0aab12c49a"

def deploy(action, game, profile):
    game, profile = game.resolve(), profile.resolve()
    require((game / "valheim.exe").is_file(), "valheim.exe missing")
    runtime = ROOT / "runtime-candidate"
    package = json.loads((runtime / "manifest.json").read_text())
    files = [(ROOT / "bin-adapter/ValheimFsr4AA.Native.dll", game / "valheim_Data/Plugins/x86_64/ValheimFsr4AA.Native.dll"),
             (ROOT / "bin-adapter/ValheimFsr4AA.Adapter.dll", profile / "BepInEx/plugins/ValheimFsr4AA/ValheimFsr4AA.Adapter.dll")]
    for entry in package["files"]:
        relative = Path(entry["path"])
        source, target = runtime / relative, game / "ValheimFsr4AA-runtime" / relative
        require(source.resolve().is_relative_to(runtime) and target.resolve().is_relative_to(game / "ValheimFsr4AA-runtime"), "Invalid runtime path")
        require(file_hash(source) == entry["sha256"].lower(), "Runtime package hash mismatch")
        files.append((source, target))
    paths = [target for _, target in files]
    for index, path in enumerate(paths):
        require(path.resolve().is_relative_to(profile if index == 1 else game), "Target escapes installation root")
    backup = game / "ValheimFsr4AA-backup-0.2.0"
    manifest = backup / "manifest.json"
    if action == "remove":
        restore(json.loads(manifest.read_text()), paths, backup)
        print("Candidate removed; 0.1.1 native restored. Foreign files/settings unchanged.")
        return
    if manifest.exists():
        state = json.loads(manifest.read_text())
        require(state["state"] == "installed" and [e["path"] for e in state["files"]] == [str(p) for p in paths] and
                all(file_hash(p) == e["after"] for p, e in zip(paths, state["files"])), "Existing candidate install differs; preserved")
        print("Candidate already installed, all hashes match")
        return
    require(not backup.exists(), "Backup already exists without manifest")
    before = [file_hash(p) for p in paths]
    previous = json.loads((game / "ValheimFsr4AA-backup-0.1.1/manifest.json").read_text())
    require(previous["state"] == "installed", "Install checked diagnostics 0.1.1 first")
    require(before[0] == previous["files"][2]["after"], "Native DLL differs from the installed own 0.1.1")
    require(before[0] in {INSTALLED_NATIVE_011, file_hash(ROOT / "bin/ValheimFsr4AA.Native.dll")}, "Expected original own diagnostics DLL")
    require(file_hash(game / "valheim_Data/globalgamemanagers") == previous["files"][0]["after"], "Preload asset changed")
    require(all(value is None for value in before[1:]), "A candidate target already exists; preserved")
    expected = {Path(e["Path"]).name: e["Hash"].lower() for e in json.loads((ROOT / "evidence/adapter-hashes.json").read_text())}
    require(all(file_hash(source) == expected[source.name] for source, _ in files[:2]), "Adapter DLL hash mismatch")
    contents = [source.read_bytes() for source, _ in files]
    state = {"state": "prepared", "files": [{"path": str(p), "before": b, "after": digest(data)} for p,b,data in zip(paths,before,contents)]}
    blockers = []
    if (game / "dxgi.dll").exists(): blockers.append("Foreign dxgi.dll present; this installer will not rename/replace it")
    config = profile / "BepInEx/config/com.wubarrk.graphicsoverdrive.cfg"
    if config.exists():
        values = dict(re.findall(r"^([A-Za-z]+)\s*=\s*([^\r\n]+)", config.read_text(encoding="utf-8-sig"), re.M))
        if values.get("MasterEnable", "true").strip().lower() == "true" and (
            values.get("ExclusiveControl", "true").strip().lower() == "true" or values.get("Scaler", "FSR3").strip() != "Vanilla"):
            blockers.append("GraphicsOverdrive: set ExclusiveControl=false and Native Scaler/Scaler=Vanilla, or disable its master")
    print(json.dumps({"changes": state, "blockers": blockers}, indent=2))
    if action == "check": return
    require(not blockers, "Resolve the printed conflicts first; no files changed")
    backup.mkdir()
    write(backup / "0.bin", paths[0].read_bytes())
    require(file_hash(backup / "0.bin") == before[0], "Native DLL changed while backing up")
    write(manifest, json.dumps(state, indent=2).encode())
    try:
        for path, checksum, data in zip(paths, before, contents):
            require(file_hash(path) == checksum, "Target changed during install")
            write(path, data)
        require(all(file_hash(p) == e["after"] for p,e in zip(paths,state["files"])), "Post-install hash mismatch")
        state["state"] = "installed"
        write(manifest, json.dumps(state, indent=2).encode())
    except Exception:
        restore(state, paths, backup)
        raise
    print("Candidate installed; disabled by default, F9 toggles. Backup:", backup)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["check", "install", "remove"])
    parser.add_argument("--game", type=Path, default=Path(r"D:\SteamLibrary\steamapps\common\Valheim"))
    parser.add_argument("--profile", type=Path, default=Path(os.environ["APPDATA"]) / "Thunderstore Mod Manager/DataFolder/Valheim/profiles/Default")
    args = parser.parse_args()
    status = subprocess.run(["powershell.exe","-NoProfile","-NonInteractive","-Command","if (Get-Process valheim -ErrorAction SilentlyContinue) { exit 10 }; exit 0"], check=False)
    require(status.returncode == 0, "Close Valheim first, or process check failed")
    deploy(args.action,args.game,args.profile)
