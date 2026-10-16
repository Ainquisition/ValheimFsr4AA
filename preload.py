"""Valheim 1.0.20 / Unity 6000.0.75f1 only. No external packages."""
import argparse
import hashlib
from pathlib import Path
import struct

ORIGINAL_SHA256 = "5161124394055da500c63e1de8098f62f3e7fb189b53873d758dee21ab37acb6"
PLUGIN = "ValheimFsr4AA.Native"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def objects(data):
    require(len(data) >= 64 and struct.unpack_from(">I", data, 8)[0] == 22,
            "Expected serialized-file format 22")
    require(data[16] == 0, "Expected little-endian metadata")
    meta_size, file_size, data_offset = struct.unpack_from(">IQQ", data, 20)
    require(file_size == len(data) and 48 + meta_size <= data_offset <= len(data), "Invalid header")
    p = data.index(0, 48)
    require(data[48:p] == b"6000.0.75f1", "Unity version mismatch")
    p += 1
    platform, tree, count = struct.unpack_from("<iBi", data, p)
    require(platform == 19 and tree == 0 and 0 < count < 1000, "Unexpected type table")
    p += 9
    types = []
    for _ in range(count):
        class_id = struct.unpack_from("<i", data, p)[0]
        types.append(class_id)
        p += 23 + (16 if class_id == 114 else 0)
    count = struct.unpack_from("<i", data, p)[0]
    require(0 < count < 1000, "Unexpected object count")
    p += 4
    entries = []
    for _ in range(count):
        p = (p + 3) & ~3
        path_id, relative, size, type_id = struct.unpack_from("<qqIi", data, p)
        require(0 <= type_id < len(types), "Invalid object type")
        start = data_offset + relative
        require(data_offset <= start <= start + size <= len(data), "Invalid object extent")
        entries.append((path_id, types[type_id], start, size, p))
        p += 24
    require(p <= 48 + meta_size, "Object table exceeds metadata")
    return data_offset, entries


def string_array(data, p):
    start = p
    count = struct.unpack_from("<I", data, p)[0]
    require(count < 1024, "Unexpected string array count")
    p += 4
    values = []
    for _ in range(count):
        size = struct.unpack_from("<I", data, p)[0]
        p += 4
        require(size < 4096 and p + size <= len(data), "Invalid string")
        values.append(data[p:p + size].decode("utf-8"))
        p = (p + size + 3) & ~3
    require(p <= len(data), "String array exceeds object")
    return start, p, values


def plugins(data):
    _, entries = objects(data)
    matches = [e for e in entries if e[1] == 141]
    require(len(matches) == 1, "Expected exactly one BuildSettings object")
    entry = matches[0]
    body = data[entry[2]:entry[2] + entry[3]]
    _, after_scenes, _ = string_array(body, 0)
    start, end, names = string_array(body, after_scenes)
    return entry, body, start, end, names


def patched(data, plugin=PLUGIN):
    require(digest(data) == ORIGINAL_SHA256, "Unknown globalgamemanagers hash; original preserved")
    offset, _ = objects(data)
    entry, body, start, end, names = plugins(data)
    require(plugin not in names, "Already registered")
    encoded = plugin.encode("utf-8")
    value = struct.pack("<I", len(encoded)) + encoded
    value += b"\0" * (-len(value) % 4)
    array = struct.pack("<I", len(names) + 1) + body[start + 4:end] + value
    replacement = body[:start] + array + body[end:]
    # Append the changed object. All other object bytes and offsets stay intact.
    new_start = (len(data) + 15) & ~15
    result = bytearray(data + b"\0" * (new_start - len(data)) + replacement)
    struct.pack_into(">Q", result, 24, len(result))
    struct.pack_into("<qI", result, entry[4] + 8, new_start - offset, len(replacement))
    result = bytes(result)
    require(plugins(result)[4] == names + [plugin], "Preload verification failed")
    old_objects = objects(data)[1]
    new_objects = objects(result)[1]
    require(len(old_objects) == len(new_objects), "Object count changed")
    for before, after in zip(old_objects, new_objects):
        require(before[:2] == after[:2], "Object identity changed")
        if before[1] != 141:
            require(before == after and data[before[2]:before[2] + before[3]] ==
                    result[after[2]:after[2] + after[3]], "Unrelated object changed")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    original = args.original.read_bytes()
    result = patched(original)
    # Exclusive create prevents accidental overwrite, including the source file.
    with args.output.open("xb") as target:
        target.write(result)
    print(f"original={digest(original)} patched={digest(result)} plugins={plugins(result)[4]}")
