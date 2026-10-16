"""User-authorized reversible switch of the existing OptiScaler/GraphicsOverdrive stack."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
from deploy import ROOT, file_hash, write
from preload import digest, require

OLD_PROXY = "fbfb6676b829dad7e020fb830586a16aa0ec6add78016db48ef12e2ae1803231"
spec = importlib.util.spec_from_file_location("candidate", ROOT / "deploy-adapter.py")
candidate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate)


def switch(action, game, profile):
    game, profile = game.resolve(), profile.resolve()
    proxy = game / "dxgi.dll"
    disabled = game / "dxgi.dll.before-vf4"
    config = profile / "BepInEx/config/com.wubarrk.graphicsoverdrive.cfg"
    backup = game / "ValheimFsr4AA-stack-backup-0.2.0"
    manifest = backup / "manifest.json"
    require(all(p.resolve().is_relative_to(game) for p in (proxy, disabled, backup)), "Proxy/backup path escapes game")
    require(config.resolve().is_relative_to(profile), "Config path escapes profile")
    if action == "restore":
        state = json.loads(manifest.read_text())
        require(state["game"] == str(game) and state["profile"] == str(profile), "Backup target mismatch")
        require(file_hash(backup / "dxgi.dll") == state["proxyHash"] and
                file_hash(backup / "GraphicsOverdrive.cfg") == state["configBefore"], "Backup checksum mismatch")
        require(file_hash(proxy) in (None, state["proxyHash"]), "Different dxgi.dll exists; preserved")
        require(file_hash(disabled) in (None, state["proxyHash"]), "Renamed DLL changed; preserved")
        if state["state"] == "restored":
            require(file_hash(proxy) == state["proxyHash"] and file_hash(config) == state["configBefore"], "Restored state differs")
            print("Already restored; original hashes match")
            return
        require(state["state"] == "installed", "Stack switch is incomplete; inspect manifest")
        # Restore is an explicit request for the original settings; retain any later edits first.
        current_config = file_hash(config)
        if current_config not in (None, state["configBefore"], state["configAfter"]):
            write(backup / ("GraphicsOverdrive-after-use-" + current_config + ".cfg"), config.read_bytes())
        # The runtime UI may change its own ini. Retain that file before resetting it for the strict uninstaller.
        install = json.loads((game / "ValheimFsr4AA-backup-0.2.0/manifest.json").read_text())
        mutable = game / "ValheimFsr4AA-runtime/OptiScaler.ini"
        for entry in install["files"]:
            target = Path(entry["path"])
            require(file_hash(target) == entry["after"] or target == mutable, "Candidate file changed independently: " + str(target))
        entry = next(e for e in install["files"] if e["path"] == str(mutable))
        if file_hash(mutable) != entry["after"]:
            require(mutable.is_file(), "Runtime ini missing")
            replacement = (ROOT / "runtime-candidate/OptiScaler.ini").read_bytes()
            require(digest(replacement) == entry["after"], "Package ini does not match installed version")
            write(backup / ("OptiScaler-after-use-" + file_hash(mutable) + ".ini"), mutable.read_bytes())
            write(mutable, replacement)
        candidate.deploy("remove", game, profile)
        write(config, (backup / "GraphicsOverdrive.cfg").read_bytes())
        if not proxy.exists():
            if disabled.exists(): disabled.rename(proxy)
            else: write(proxy, (backup / "dxgi.dll").read_bytes())
        require(file_hash(proxy) == state["proxyHash"] and file_hash(config) == state["configBefore"], "Restore verification failed")
        state["state"] = "restored"
        write(manifest, json.dumps(state, indent=2).encode())
        print("RESTORED original proxy, GraphicsOverdrive config and own diagnostics 0.1.1; backups retained:", backup)
        return
    require(not backup.exists(), "Stack backup already exists; preserved")
    require(file_hash(proxy) == OLD_PROXY, "Old proxy identity changed; preserved")
    require(not disabled.exists(), "Renamed proxy target already exists; preserved")
    require(not (game / "ValheimFsr4AA-backup-0.2.0").exists(), "Candidate backup already exists; preserved")
    original = config.read_bytes()
    updated, exclusive_count = re.subn(rb"(?m)^(ExclusiveControl[ \t]*=[ \t]*)true([ \t]*\r?)$", rb"\g<1>false\2", original)
    updated, scaler_count = re.subn(rb"(?m)^(Scaler[ \t]*=[ \t]*)FSR3([ \t]*\r?)$", rb"\g<1>Vanilla\2", updated)
    require(exclusive_count == 1 and scaler_count == 1, "Expected exactly two known GraphicsOverdrive settings")
    state = {"state":"prepared", "game":str(game), "profile":str(profile), "proxyHash":OLD_PROXY,
             "configBefore":digest(original), "configAfter":digest(updated)}
    backup.mkdir()
    write(backup / "dxgi.dll", proxy.read_bytes())
    write(backup / "GraphicsOverdrive.cfg", original)
    require(file_hash(backup / "dxgi.dll") == OLD_PROXY and file_hash(config) == state["configBefore"], "Source changed while backing up")
    write(manifest, json.dumps(state, indent=2).encode())
    try:
        require(file_hash(proxy) == OLD_PROXY and not disabled.exists(), "Proxy changed during preparation")
        proxy.rename(disabled)
        require(file_hash(config) == state["configBefore"], "Config changed during preparation")
        write(config, updated)
        candidate.deploy("install", game, profile)
        require(not proxy.exists() and file_hash(disabled) == OLD_PROXY and file_hash(config) == state["configAfter"], "Switch verification failed")
        state["state"] = "installed"
        write(manifest, json.dumps(state, indent=2).encode())
    except Exception:
        if file_hash(config) == state["configAfter"]: write(config, original)
        if not proxy.exists() and file_hash(disabled) == OLD_PROXY: disabled.rename(proxy)
        state["state"] = "failed"
        write(manifest, json.dumps(state, indent=2).encode())
        raise
    print("SWITCHED and installed candidate. Original proxy/config verified in", backup)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["install","restore"])
    parser.add_argument("--game", type=Path, default=Path(r"D:\SteamLibrary\steamapps\common\Valheim"))
    parser.add_argument("--profile", type=Path, default=Path(os.environ["APPDATA"]) / "Thunderstore Mod Manager/DataFolder/Valheim/profiles/Default")
    args = parser.parse_args()
    status = subprocess.run(["powershell.exe","-NoProfile","-NonInteractive","-Command","if (Get-Process valheim -ErrorAction SilentlyContinue) { exit 10 }; exit 0"], check=False)
    require(status.returncode == 0, "Close Valheim first, or process check failed")
    switch(args.action,args.game,args.profile)
