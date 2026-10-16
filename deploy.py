"""Install/remove this diagnostic only. Preserves unknown files and retains backups."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
from preload import digest, patched, require, ORIGINAL_SHA256

ROOT = Path(__file__).resolve().parent
OLD_MANAGED = "C287ACAF52E57D59285E4F153BA58F1844C2145E25920D399BA4635EF3168DBF".lower()
OLD_NATIVE = "AB33943820AEA0E93EA0ADCD208CDB7071D349CA5E690A00B8901235949D9EE6".lower()


def file_hash(path):
    return digest(path.read_bytes()) if path.is_file() else None


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    name = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".ValheimFsr4AA-", delete=False) as tmp:
            name = Path(tmp.name)
            tmp.write(data)
            tmp.flush()
            os.fsync(tmp.fileno())
        os.replace(name, path)
    finally:
        if name is not None and name.exists():
            name.unlink()


def target_paths(game, profile):
    paths = [game / "valheim_Data/globalgamemanagers",
             profile / "BepInEx/plugins/ValheimFsr4AA/ValheimFsr4AA.Diagnostics.dll",
             game / "valheim_Data/Plugins/x86_64/ValheimFsr4AA.Native.dll",
             game / "ValheimFsr4AA.Native.dll"]
    for path, root in zip(paths, [game, profile, game, game]):
        require(path.resolve().is_relative_to(root), f"Target escapes installation root: {path}")
    return paths


def verify_manifest(state, paths):
    require([e["path"] for e in state["files"]] == [str(p) for p in paths], "Backup target mismatch")


def restore(state, paths, backup):
    verify_manifest(state, paths)
    # Validate the entire transaction before restoring anything.
    for index, (entry, path) in enumerate(zip(state["files"], paths)):
        require(file_hash(path) in (entry["before"], entry["after"]), f"File changed independently: {path}")
        if entry["before"] is not None:
            require(file_hash(backup / f"{index}.bin") == entry["before"], "Backup checksum mismatch")
    for index, (entry, path) in enumerate(zip(state["files"], paths)):
        if entry["before"] is None:
            if path.exists():
                path.unlink()  # Only a verified file created by this installer.
        else:
            write(path, (backup / f"{index}.bin").read_bytes())
    state["state"] = "removed"
    write(backup / "manifest.json", json.dumps(state, indent=2).encode())


def deploy(action, game, profile):
    game, profile = game.resolve(), profile.resolve()
    require((game / "valheim.exe").is_file(), "valheim.exe not found")
    paths = target_paths(game, profile)
    backup = game / "ValheimFsr4AA-backup-0.1.1"
    require(backup.resolve().is_relative_to(game), "Backup escapes installation root")
    manifest = backup / "manifest.json"
    if action == "remove":
        state = json.loads(manifest.read_text())
        restore(state, paths, backup)
        print("REMOVED: original files restored; backup retained at", backup)
        return
    if manifest.exists():
        state = json.loads(manifest.read_text())
        verify_manifest(state, paths)
        require(state["state"] == "installed" and all(file_hash(p) == e["after"]
                for p, e in zip(paths, state["files"])), "Existing backup/install differs; preserved")
        print("ALREADY INSTALLED; all target hashes match")
        return
    require(not backup.exists(), "Backup directory already exists without manifest; preserved")
    native = (ROOT / "bin/ValheimFsr4AA.Native.dll").read_bytes()
    managed = (ROOT / "bin/ValheimFsr4AA.Diagnostics.dll").read_bytes()
    hashes = {entry["File"]: entry["Hash"].lower() for entry in json.loads((ROOT / "evidence/hashes.json").read_text())}
    require(digest(native) == hashes["ValheimFsr4AA.Native.dll"] and
            digest(managed) == hashes["ValheimFsr4AA.Diagnostics.dll"], "Package DLL checksum mismatch")
    before = [file_hash(path) for path in paths]
    allowed = [{ORIGINAL_SHA256}, {None, OLD_MANAGED}, {None}, {None, OLD_NATIVE}]
    for path, checksum, expected in zip(paths, before, allowed):
        require(checksum in expected, f"Unknown existing file preserved: {path} ({checksum})")
    replacements = [patched(paths[0].read_bytes()), managed, native, None]
    state = {"state": "prepared", "files": [
        {"path": str(path), "before": checksum, "after": digest(data) if data is not None else None}
        for path, checksum, data in zip(paths, before, replacements)]}
    print(json.dumps(state, indent=2))
    if action == "check":
        return
    backup.mkdir()
    for index, (path, checksum) in enumerate(zip(paths, before)):
        if checksum is not None:
            data = path.read_bytes()
            require(digest(data) == checksum, "Target changed while backing up")
            write(backup / f"{index}.bin", data)
    write(manifest, json.dumps(state, indent=2).encode())
    try:
        for path, checksum, data in zip(paths, before, replacements):
            require(file_hash(path) == checksum, f"Target changed during installation: {path}")
            if data is None:
                if path.exists():
                    path.unlink()  # Only the recognized old native DLL, already backed up.
            else:
                write(path, data)
        require(all(file_hash(p) == e["after"] for p, e in zip(paths, state["files"])), "Post-install verification failed")
        state["state"] = "installed"
        write(manifest, json.dumps(state, indent=2).encode())
    except Exception:
        restore(state, paths, backup)
        raise
    print("INSTALLED 0.1.1; verified every target; backup:", backup)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["check", "install", "remove"])
    parser.add_argument("--game", type=Path, default=Path(r"D:\SteamLibrary\steamapps\common\Valheim"))
    parser.add_argument("--profile", type=Path, default=Path(os.environ["APPDATA"]) / "Thunderstore Mod Manager/DataFolder/Valheim/profiles/Default")
    args = parser.parse_args()
    check = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
        "if (Get-Process -Name valheim -ErrorAction SilentlyContinue) { exit 10 }; exit 0"], check=False)
    require(check.returncode == 0, "Close Valheim first, or process-state check failed")
    deploy(args.action, args.game, args.profile)
