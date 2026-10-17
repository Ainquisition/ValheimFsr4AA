"""Upgrade only our two DLLs; keep the original full-stack rollback intact."""
import argparse
import copy
import json
import os
import re
from pathlib import Path
import subprocess
from deploy import ROOT, file_hash, write
from preload import require


def upgrade(game, profile):
    game, profile = game.resolve(), profile.resolve()
    active = game / "ValheimFsr4AA-backup-0.2.0/manifest.json"
    original = active.read_bytes()
    state = json.loads(original)
    paths = [game / "valheim_Data/Plugins/x86_64/ValheimFsr4AA.Native.dll",
             profile / "BepInEx/plugins/ValheimFsr4AA/ValheimFsr4AA.Adapter.dll"]
    sources = [ROOT / "bin-adapter" / path.name for path in paths]
    expected = {Path(e["Path"]).name: e["Hash"].lower() for e in
                json.loads((ROOT / "evidence/adapter-hashes.json").read_text())}
    require(state["state"] == "installed", "Candidate not installed")
    require([e["path"] for e in state["files"][:2]] == [str(p) for p in paths], "Target mismatch")
    require(all(file_hash(p) == e["after"] for p, e in zip(paths, state["files"])), "Installed DLL changed independently")
    require(all(file_hash(p) == expected[p.name] for p in sources), "Package hash mismatch")
    contents = [p.read_bytes() for p in sources]
    updated = copy.deepcopy(state)
    for entry, source in zip(updated["files"], sources):
        entry["after"] = expected[source.name]
    ini = game / "ValheimFsr4AA-runtime/OptiScaler.ini"
    original_ini = ini.read_bytes()
    updated_ini, replacements = re.subn(rb"(?m)^OptiDllPath\s*=\s*[^\r\n]*", b"OptiDllPath = ValheimFsr4AA-runtime/OptiScaler", original_ini)
    require(replacements == 1, "Expected exactly one OptiDllPath setting")
    ini_entries = [entry for entry in updated["files"] if entry["path"] == str(ini)]
    require(len(ini_entries) == 1, "Runtime ini is not owned by this installation")
    # The UI can edit the live ini. Full-stack restore first restores the packaged ini.
    ini_entries[0]["after"] = file_hash(ROOT / "runtime-candidate/OptiScaler.ini")
    if updated == state and updated_ini == original_ini:
        print("Already upgraded; both DLL hashes match")
        return
    backup = game / "ValheimFsr4AA-backup-0.2.2"
    require(not backup.exists(), "Upgrade backup already exists; preserved")
    saved = [p.read_bytes() for p in paths]
    backup.mkdir()
    for index, data in enumerate(saved):
        write(backup / f"{index}.bin", data)
        require(file_hash(backup / f"{index}.bin") == state["files"][index]["after"], "Backup hash mismatch")
    write(backup / "active-manifest-before.json", original)
    write(backup / "OptiScaler-before.ini", original_ini)
    require((backup / "active-manifest-before.json").read_bytes() == original, "Manifest backup mismatch")
    try:
        require(active.read_bytes() == original, "Manifest changed during backup")
        require(ini.read_bytes() == original_ini, "Runtime ini changed during backup")
        for path, data, entry in zip(paths, contents, state["files"]):
            require(file_hash(path) == entry["after"], "DLL changed during backup")
            write(path, data)
        require(all(file_hash(p) == e["after"] for p, e in zip(paths, updated["files"])), "Post-upgrade hash mismatch")
        write(ini, updated_ini)
        write(active, json.dumps(updated, indent=2).encode())
        write(backup / "manifest.json", json.dumps({"state": "installed", "previous": state["files"][:2],
              "current": updated["files"][:2], "ini": str(ini)}, indent=2).encode())
    except Exception:
        for path, data in zip(paths, saved):
            write(path, data)
        write(active, original)
        write(ini, original_ini)
        raise
    print("Upgraded two own DLLs. Previous DLLs and manifest:", backup)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=Path(r"D:\SteamLibrary\steamapps\common\Valheim"))
    parser.add_argument("--profile", type=Path, default=Path(os.environ["APPDATA"]) / "Thunderstore Mod Manager/DataFolder/Valheim/profiles/Default")
    args = parser.parse_args()
    status = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
        "if (Get-Process valheim -ErrorAction SilentlyContinue) { exit 10 }; exit 0"], check=False)
    require(status.returncode == 0, "Close Valheim first, or process check failed")
    upgrade(args.game, args.profile)
