"""Read-only test against the matching original globalgamemanagers."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import preload

data = Path(sys.argv[1]).read_bytes()
result = preload.patched(data)
assert preload.plugins(result)[4] == preload.plugins(data)[4] + [preload.PLUGIN]
# Other content remains at the same positions; only size and one object-table entry may differ.
entry = preload.plugins(data)[0]
restored_prefix = bytearray(result[:len(data)])
restored_prefix[24:32] = data[24:32]
restored_prefix[entry[4] + 8:entry[4] + 20] = data[entry[4] + 8:entry[4] + 20]
assert bytes(restored_prefix) == data
for invalid in (data[:-1], data[:80] + bytes([data[80] ^ 1]) + data[81:], result):
    try:
        preload.patched(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError("Unknown/already-patched file was accepted")
print("PASS preload: appended registration, unrelated bytes unchanged, incompatible inputs refused")
