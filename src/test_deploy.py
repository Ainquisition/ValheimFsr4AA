"""Exercise installation and rollback in a new workspace folder, never in the game."""
from pathlib import Path
import shutil
import sys
import uuid
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import deploy

source_game, source_profile, scratch = (Path(value).resolve() for value in sys.argv[1:4])
scratch = scratch.resolve() / ("deploy-test-" + uuid.uuid4().hex)
scratch.mkdir()
game, profile = scratch / "Valheim", scratch / "Profile"
paths = deploy.target_paths(game, profile)
(game / "valheim.exe").parent.mkdir()
(game / "valheim.exe").write_bytes(b"TEST ONLY, NEVER EXECUTED")
originals = deploy.target_paths(source_game, source_profile)
for index in (0, 1, 3):
    paths[index].parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(originals[index], paths[index])
before = [deploy.file_hash(p) for p in paths]
deploy.deploy("check", game, profile)
assert before == [deploy.file_hash(p) for p in paths]
deploy.deploy("install", game, profile)
deploy.deploy("install", game, profile)
# A changed target must prevent removal before any other file is touched.
original_managed = paths[1].read_bytes()
paths[1].write_bytes(b"independent change")
changed = [deploy.file_hash(p) for p in paths]
try:
    deploy.deploy("remove", game, profile)
except ValueError:
    pass
else:
    raise AssertionError("Modified target was overwritten")
assert changed == [deploy.file_hash(p) for p in paths]
paths[1].write_bytes(original_managed)
deploy.deploy("remove", game, profile)
assert before == [deploy.file_hash(p) for p in paths]
print("PASS deploy: dry run, install, idempotence, independent changes refused, exact rollback")
