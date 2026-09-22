#!/usr/bin/python3
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
"""Read Android liblp metadata and print safe dm-linear tables.

The Chiron stores its dynamic partitions in /dev/mmcblk0p22.  This parser is
deliberately small and read-only: it validates both liblp checksums, accepts
only linear/zero extents, and emits data for the privileged setup helper.  It
does not invoke dmsetup and never writes the super partition.
"""

import argparse
import hashlib
import json
import os
import subprocess
import struct
import sys


GEOMETRY_MAGIC = 0x616C4467
HEADER_MAGIC = 0x414C5030
RESERVED_BYTES = 4096
GEOMETRY_SIZE = 4096
SECTOR_SIZE = 512
TARGET_LINEAR = 0
TARGET_ZERO = 1


class MetadataError(RuntimeError):
    pass


def _read_exact(stream, offset, size):
    stream.seek(offset)
    data = stream.read(size)
    if len(data) != size:
        raise MetadataError(f"short read at {offset}: wanted {size}, got {len(data)}")
    return data


def _cstring(raw):
    return raw.split(b"\0", 1)[0].decode("ascii", "strict")


def _geometry_at(stream, offset):
    raw = _read_exact(stream, offset, GEOMETRY_SIZE)
    magic, struct_size = struct.unpack_from("<II", raw)
    if magic != GEOMETRY_MAGIC:
        raise MetadataError("bad geometry magic")
    if not 52 <= struct_size <= GEOMETRY_SIZE:
        raise MetadataError(f"bad geometry size {struct_size}")
    checked = bytearray(raw[:struct_size])
    expected = bytes(checked[8:40])
    checked[8:40] = bytes(32)
    if hashlib.sha256(checked).digest() != expected:
        raise MetadataError("bad geometry checksum")
    metadata_max_size, slot_count, logical_block_size = struct.unpack_from(
        "<III", raw, 40
    )
    if metadata_max_size < 512 or metadata_max_size % SECTOR_SIZE:
        raise MetadataError(f"bad metadata_max_size {metadata_max_size}")
    if not 1 <= slot_count <= 4:
        raise MetadataError(f"bad metadata slot count {slot_count}")
    if logical_block_size < 4096 or logical_block_size % SECTOR_SIZE:
        raise MetadataError(f"bad logical block size {logical_block_size}")
    return metadata_max_size, slot_count, logical_block_size


def read_geometry(stream):
    errors = []
    for offset in (RESERVED_BYTES, RESERVED_BYTES + GEOMETRY_SIZE):
        try:
            return _geometry_at(stream, offset)
        except (MetadataError, UnicodeError, struct.error) as exc:
            errors.append(str(exc))
    raise MetadataError("both geometry copies are invalid: " + "; ".join(errors))


def _descriptor(raw, offset):
    table_offset, count, entry_size = struct.unpack_from("<III", raw, offset)
    return {"offset": table_offset, "count": count, "entry_size": entry_size}


def _metadata_at(stream, offset, max_size):
    prefix = _read_exact(stream, offset, 128)
    magic, major, minor, header_size = struct.unpack_from("<IHHI", prefix)
    if magic != HEADER_MAGIC:
        raise MetadataError("bad metadata magic")
    if major != 10 or minor > 2:
        raise MetadataError(f"unsupported metadata version {major}.{minor}")
    if not 128 <= header_size <= max_size:
        raise MetadataError(f"bad header size {header_size}")
    header = bytearray(_read_exact(stream, offset, header_size))
    expected_header = bytes(header[12:44])
    header[12:44] = bytes(32)
    if hashlib.sha256(header).digest() != expected_header:
        raise MetadataError("bad header checksum")
    tables_size = struct.unpack_from("<I", header, 44)[0]
    tables_checksum = bytes(header[48:80])
    if tables_size > max_size - header_size:
        raise MetadataError(f"tables exceed metadata slot: {tables_size}")
    tables = _read_exact(stream, offset + header_size, tables_size)
    if hashlib.sha256(tables).digest() != tables_checksum:
        raise MetadataError("bad tables checksum")

    descriptors = {
        "partitions": _descriptor(header, 80),
        "extents": _descriptor(header, 92),
        "groups": _descriptor(header, 104),
        "block_devices": _descriptor(header, 116),
    }
    minimum_sizes = {"partitions": 52, "extents": 24, "groups": 48, "block_devices": 64}
    for name, desc in descriptors.items():
        if desc["entry_size"] < minimum_sizes[name]:
            raise MetadataError(f"short {name} entry size {desc['entry_size']}")
        end = desc["offset"] + desc["count"] * desc["entry_size"]
        if desc["offset"] > tables_size or end > tables_size:
            raise MetadataError(f"{name} table exceeds tables blob")

    def entries(name):
        desc = descriptors[name]
        for index in range(desc["count"]):
            start = desc["offset"] + index * desc["entry_size"]
            yield tables[start : start + desc["entry_size"]]

    block_devices = []
    for raw in entries("block_devices"):
        first_sector, alignment, alignment_offset, size = struct.unpack_from("<QIIQ", raw)
        name = _cstring(raw[24:60])
        flags = struct.unpack_from("<I", raw, 60)[0]
        block_devices.append({
            "name": name,
            "first_logical_sector": first_sector,
            "alignment": alignment,
            "alignment_offset": alignment_offset,
            "size": size,
            "flags": flags,
        })

    extents = []
    for raw in entries("extents"):
        sectors, target_type, target_data, target_source = struct.unpack_from("<QIQI", raw)
        if not sectors:
            raise MetadataError("zero-length extent")
        if target_type not in (TARGET_LINEAR, TARGET_ZERO):
            raise MetadataError(f"unsupported extent target {target_type}")
        if target_type == TARGET_LINEAR and target_source >= len(block_devices):
            raise MetadataError(f"bad extent source {target_source}")
        extents.append({
            "sectors": sectors,
            "target_type": target_type,
            "target_data": target_data,
            "target_source": target_source,
        })

    partitions = []
    seen = set()
    for raw in entries("partitions"):
        name = _cstring(raw[:36])
        attributes, first_extent, extent_count, group_index = struct.unpack_from("<IIII", raw, 36)
        if not name or name in seen:
            raise MetadataError(f"bad or duplicate partition name {name!r}")
        if not all(char.isalnum() or char in "_-" for char in name):
            raise MetadataError(f"unsafe partition name {name!r}")
        if first_extent + extent_count > len(extents):
            raise MetadataError(f"partition {name} exceeds extent table")
        seen.add(name)
        partitions.append({
            "name": name,
            "attributes": attributes,
            "group_index": group_index,
            "extents": extents[first_extent : first_extent + extent_count],
        })
    return {"version": f"{major}.{minor}", "block_devices": block_devices, "partitions": partitions}


def read_metadata(stream):
    max_size, slot_count, logical_block_size = read_geometry(stream)
    base = RESERVED_BYTES + 2 * GEOMETRY_SIZE
    errors = []
    # Prefer slot 0's primary copy, then the remaining primary copies, then
    # backups. Chiron is a non-A/B device, but scanning all declared copies
    # makes a torn primary metadata update recoverable without writing it.
    offsets = [base + slot * max_size for slot in range(slot_count)]
    offsets += [base + (slot_count + slot) * max_size for slot in range(slot_count)]
    for offset in offsets:
        try:
            metadata = _metadata_at(stream, offset, max_size)
            metadata.update({
                "metadata_offset": offset,
                "metadata_max_size": max_size,
                "metadata_slot_count": slot_count,
                "logical_block_size": logical_block_size,
            })
            return metadata
        except (MetadataError, UnicodeError, struct.error) as exc:
            errors.append(f"{offset}: {exc}")
    raise MetadataError("no valid metadata copy: " + "; ".join(errors))


def add_dm_tables(metadata, device, device_size=None):
    if device_size is None:
        device_size = os.stat(device).st_size
    if not device_size:
        # st_size is normally zero for a block device. BLKGETSIZE64 avoids a
        # dependency on lsblk/blkid output formats.
        import fcntl
        import array
        size = array.array("Q", [0])
        with open(device, "rb", buffering=0) as block_device:
            fcntl.ioctl(block_device, 0x80081272, size, True)
        device_size = size[0]
    result = []
    for partition in metadata["partitions"]:
        logical_sector = 0
        table = []
        for extent in partition["extents"]:
            sectors = extent["sectors"]
            if extent["target_type"] == TARGET_ZERO:
                table.append(f"{logical_sector} {sectors} zero")
            else:
                source = metadata["block_devices"][extent["target_source"]]
                if source["name"] != "super":
                    raise MetadataError(
                        f"partition {partition['name']} uses unsupported block device {source['name']}"
                    )
                end = (extent["target_data"] + sectors) * SECTOR_SIZE
                if end > device_size:
                    raise MetadataError(f"partition {partition['name']} extent exceeds super")
                table.append(
                    f"{logical_sector} {sectors} linear {device} {extent['target_data']}"
                )
            logical_sector += sectors
        partition["sectors"] = logical_sector
        partition["dm_table"] = table
        result.append(partition)
    metadata["partitions"] = result
    return metadata


VISIBLE_PARTITIONS = ("system", "system_ext", "vendor", "product", "odm")


def create_mappings(metadata, device):
    device_number = os.stat(device).st_rdev
    kernel_device = f"{os.major(device_number)}:{os.minor(device_number)}"
    by_name = {partition["name"]: partition for partition in metadata["partitions"]}
    missing = [
        name for name in VISIBLE_PARTITIONS
        if name not in by_name or not by_name[name]["dm_table"]
    ]
    if missing:
        raise MetadataError("expected logical partitions are missing: " + ", ".join(missing))
    created = []
    for name in VISIBLE_PARTITIONS:
        partition = by_name.get(name)
        mapper_name = f"android-{name.replace('_', '-')}-ro"
        expected = "\n".join(partition["dm_table"]).replace(device, kernel_device)
        info = subprocess.run(
            ["dmsetup", "info", mapper_name], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, check=False
        )
        if info.returncode == 0:
            current = subprocess.run(
                ["dmsetup", "table", mapper_name], check=True, text=True,
                stdout=subprocess.PIPE
            ).stdout.strip()
            if current != expected:
                raise MetadataError(f"existing mapper {mapper_name} has an unexpected table")
        else:
            subprocess.run(
                ["dmsetup", "create", mapper_name, "--readonly"],
                input="\n".join(partition["dm_table"]) + "\n",
                text=True, check=True
            )
        mapper = f"/dev/mapper/{mapper_name}"
        readonly = subprocess.run(
            ["blockdev", "--getro", mapper], check=True, text=True,
            stdout=subprocess.PIPE
        ).stdout.strip()
        if readonly != "1":
            raise MetadataError(f"mapper {mapper_name} is not read-only")
        created.append({"partition": name, "mapper": mapper})
    json.dump(created, sys.stdout, sort_keys=True)
    sys.stdout.write("\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--create", action="store_true")
    parser.add_argument("device", nargs="?", default="/dev/mmcblk0p22")
    args = parser.parse_args()
    if args.device != "/dev/mmcblk0p22":
        raise MetadataError("only the Chiron super partition is accepted")
    with open(args.device, "rb", buffering=0) as stream:
        metadata = add_dm_tables(read_metadata(stream), args.device)
    if args.create:
        if os.geteuid() != 0:
            raise MetadataError("--create requires root")
        create_mappings(metadata, args.device)
    else:
        json.dump(metadata, sys.stdout, sort_keys=True)
        sys.stdout.write("\n")


if __name__ == "__main__":
    try:
        main()
    except (MetadataError, OSError, subprocess.SubprocessError) as exc:
        print(f"chiron-super: {exc}", file=sys.stderr)
        sys.exit(1)
