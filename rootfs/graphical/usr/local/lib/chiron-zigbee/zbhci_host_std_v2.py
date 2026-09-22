#!/usr/bin/env python3
# SPDX-License-Identifier: 0BSD
# Copyright (c) 2026 angellfear
"""Telink ZBHCI host for the Chiron onboard TLSR9218/B91 coordinator.

Wire format (Telink ZBHCI UART):
    55 <msg_type:2 BE> <payload_len:2 BE> <xor:1> <payload...> AA

The checksum is XOR(message type bytes, length bytes, payload bytes).
All HCI fields are big-endian according to the Telink Zigbee SDK manual.

This is intentionally a diagnostic/bring-up host, not yet a Zigbee2MQTT adapter.
Factory-reset and leave remain out of the CLI. Network formation exists only as
an explicit state-changing command and is never run by selftest.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from typing import Iterable, Optional

try:
    import serial
except ImportError:
    serial = None

GPIOCHIP = "gpiochip0"
DEFAULT_PORT = "/dev/ttyS3"
DEFAULT_BAUD = 115200
CTRL_SOCK = "/run/zbhci_host.sock"

# Standard Telink ZBHCI message types used by this host.
ACK = 0x8000
BDB_FORMATION_REQ = 0x0001
BDB_CHANNEL_SET_REQ = 0x0007
NODE_DESC_REQ = 0x0012
SIMPLE_DESC_REQ = 0x0013
ACTIVE_EP_REQ = 0x0015
MGMT_LQI_REQ = 0x0030
MGMT_PERMIT_JOIN_REQ = 0x0034
NODES_JOINED_GET_REQ = 0x0040
GET_LOCAL_NWK_INFO_REQ = 0x0045
ZCL_ATTR_READ_REQ = 0x0100
ZCL_ATTR_READ_RSP = 0x8100
ZCL_ONOFF_ON = 0x0140
ZCL_ONOFF_OFF = 0x0141
ZCL_ONOFF_TOGGLE = 0x0142
APS_SEND_REQ = 0x0901
APS_SEND_RSP = 0x8901

# Yandex/current-firmware extensions observed live. These are not required for
# the standard probe, but are useful for comparison with the old PoC.
VENDOR_VERSION_REQ = 0x0900
VENDOR_UNKNOWN_0908_REQ = 0x0908
VENDOR_BOOT_IEEE_IND = 0x9008

ACK_STATUS = {
    0: "success",
    1: "wrong_parameter",
    2: "unsupported_command",
    3: "busy",
    4: "no_memory",
}


def parse_int(value: str) -> int:
    return int(value, 0)


def u16(value: int) -> bytes:
    if not 0 <= value <= 0xFFFF:
        raise ValueError(f"u16 out of range: {value}")
    return struct.pack(">H", value)


def gpio_set(pin: int, val: int) -> None:
    # `gpioset <chip> <line>=<value>` is libgpiod v1. This image ships
    # libgpiod 2 -- and only libgpiod 2: Ubuntu 26.04 carries no v1 package at
    # all -- so the v1 form fails outright. The sequence below is unchanged;
    # only the spelling of the call follows the tool that is installed.
    #
    # Two things change between the versions, not one. The chip becomes an
    # option (a bare `gpiochip0` in that position is read as a line
    # specification instead), and the default mode inverts: v1 set the value
    # and returned, v2 holds the line and waits for a signal -- "the line
    # output state is maintained until the process exits". Left at the v2
    # default this call never returns, and a Type=oneshot unit that never
    # returns holds multi-user.target for the whole boot. `--toggle 0` is v2's
    # spelling of v1's default --mode=exit: set, then exit immediately.
    subprocess.run(
        ["gpioset", "--chip", GPIOCHIP, "--toggle", "0", f"{pin}={val}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )


def power_on() -> None:
    # Preserve the sequence already proven on Chiron.
    gpio_set(154, 1)  # normal boot
    gpio_set(150, 0)
    time.sleep(0.05)
    gpio_set(150, 1)
    gpio_set(153, 0)
    time.sleep(0.05)
    gpio_set(153, 1)
    time.sleep(0.4)


def xor_checksum(msg_type: int, payload: bytes = b"") -> int:
    hdr = struct.pack(">HH", msg_type, len(payload))
    x = 0
    for b in hdr:
        x ^= b
    for b in payload:
        x ^= b
    return x


def build_frame(msg_type: int, payload: bytes = b"") -> bytes:
    if not 0 <= msg_type <= 0xFFFF:
        raise ValueError("message type must fit in 16 bits")
    if len(payload) > 0xFFFF:
        raise ValueError("payload too large")
    hdr = struct.pack(">HH", msg_type, len(payload))
    return b"\x55" + hdr + bytes([xor_checksum(msg_type, payload)]) + payload + b"\xaa"


@dataclass(frozen=True)
class Frame:
    msg_type: int
    payload: bytes
    checksum: int
    checksum_ok: bool
    raw: bytes

    def as_dict(self) -> dict:
        d = {
            "type": f"0x{self.msg_type:04x}",
            "length": len(self.payload),
            "checksum": f"0x{self.checksum:02x}",
            "checksum_ok": self.checksum_ok,
            "payload": self.payload.hex(),
            "raw": self.raw.hex(),
        }
        decoded = decode_frame(self)
        if decoded:
            d["decoded"] = decoded
        return d


class FrameParser:
    """Incremental ZBHCI parser; payload may legally contain 0xAA or 0x55."""

    def __init__(self) -> None:
        self.buf = bytearray()

    def feed(self, data: bytes) -> list[Frame]:
        self.buf.extend(data)
        out: list[Frame] = []

        while True:
            # Resync to header.
            try:
                start = self.buf.index(0x55)
            except ValueError:
                self.buf.clear()
                break
            if start:
                del self.buf[:start]

            # header + type + length + checksum = 6 bytes before payload
            if len(self.buf) < 6:
                break

            msg_type = (self.buf[1] << 8) | self.buf[2]
            payload_len = (self.buf[3] << 8) | self.buf[4]
            total_len = 7 + payload_len
            if len(self.buf) < total_len:
                break

            if self.buf[total_len - 1] != 0xAA:
                # False header/corruption. Drop only the header byte and resync.
                del self.buf[0]
                continue

            raw = bytes(self.buf[:total_len])
            del self.buf[:total_len]
            checksum = raw[5]
            payload = raw[6:-1]
            expected = xor_checksum(msg_type, payload)
            out.append(
                Frame(
                    msg_type=msg_type,
                    payload=payload,
                    checksum=checksum,
                    checksum_ok=(checksum == expected),
                    raw=raw,
                )
            )

        return out


def be16(data: bytes, off: int = 0) -> int:
    return struct.unpack_from(">H", data, off)[0]


def fmt_ieee(data: bytes) -> str:
    return data.hex(":")


def decode_ack(payload: bytes) -> Optional[dict]:
    if len(payload) < 4:
        return None
    req = be16(payload, 0)
    status = payload[2]
    return {
        "kind": "ack",
        "request_type": f"0x{req:04x}",
        "status": status,
        "status_name": ACK_STATUS.get(status, "unknown"),
        "reserved": payload[3],
    }


def decode_netinfo(payload: bytes) -> Optional[dict]:
    # Telink documented 23-byte ZBHCI_CMD_GET_LOCAL_NWK_INFO_RSP.
    if len(payload) != 23:
        return None
    return {
        "kind": "local_network_info",
        "device_type": payload[0],
        "capability": f"0x{payload[1]:02x}",
        "on_network": bool(payload[2]),
        "pan_id": f"0x{be16(payload, 3):04x}",
        "ext_pan_id": fmt_ieee(payload[5:13]),
        "nwk_addr": f"0x{be16(payload, 13):04x}",
        "ieee_addr": fmt_ieee(payload[15:23]),
    }


def decode_zdo_rsp(payload: bytes, kind: str) -> Optional[dict]:
    if len(payload) < 4:
        return None
    return {
        "kind": kind,
        "src_addr": f"0x{be16(payload, 0):04x}",
        "seq": payload[2],
        "status": payload[3],
    }


def decode_active_ep(payload: bytes) -> Optional[dict]:
    if len(payload) < 7:
        return decode_zdo_rsp(payload, "active_ep_rsp")
    count = payload[6]
    eps = list(payload[7:7 + count])
    return {
        "kind": "active_ep_rsp",
        "src_addr": f"0x{be16(payload, 0):04x}",
        "seq": payload[2],
        "status": payload[3],
        "nwk_addr": f"0x{be16(payload, 4):04x}",
        "endpoint_count": count,
        "endpoints": eps,
    }


def decode_simple_desc(payload: bytes) -> Optional[dict]:
    if len(payload) < 7:
        return decode_zdo_rsp(payload, "simple_desc_rsp")
    n = payload[6]
    desc = payload[7:7 + n]
    return {
        "kind": "simple_desc_rsp",
        "src_addr": f"0x{be16(payload, 0):04x}",
        "seq": payload[2],
        "status": payload[3],
        "nwk_addr": f"0x{be16(payload, 4):04x}",
        "descriptor_length": n,
        "descriptor_raw": desc.hex(),
    }


def decode_node_desc(payload: bytes) -> Optional[dict]:
    if len(payload) < 6:
        return decode_zdo_rsp(payload, "node_desc_rsp")
    return {
        "kind": "node_desc_rsp",
        "src_addr": f"0x{be16(payload, 0):04x}",
        "seq": payload[2],
        "status": payload[3],
        "nwk_addr": f"0x{be16(payload, 4):04x}",
        "descriptor_raw": payload[6:].hex(),
    }


def decode_lqi(payload: bytes) -> Optional[dict]:
    if len(payload) < 7:
        return decode_zdo_rsp(payload, "mgmt_lqi_rsp")
    count = payload[6]
    raw_entries = payload[7:]
    entries = []
    # Zigbee Neighbor Table descriptor is 22 bytes. Keep raw if firmware varies.
    for i in range(count):
        e = raw_entries[i * 22:(i + 1) * 22]
        if len(e) != 22:
            break
        entries.append({
            "ext_pan_id": fmt_ieee(e[0:8]),
            "ieee_addr": fmt_ieee(e[8:16]),
            "nwk_addr": f"0x{be16(e, 16):04x}",
            "device_bits": f"0x{e[18]:02x}",
            "permit_joining": e[19],
            "depth": e[20],
            "lqi": e[21],
        })
    return {
        "kind": "mgmt_lqi_rsp",
        "src_addr": f"0x{be16(payload, 0):04x}",
        "seq": payload[2],
        "status": payload[3],
        "total_entries": payload[4],
        "start_index": payload[5],
        "list_count": count,
        "entries": entries,
        "entries_raw": raw_entries.hex(),
    }


def decode_joined(payload: bytes) -> Optional[dict]:
    if len(payload) < 6:
        return None
    total = be16(payload, 0)
    start = be16(payload, 2)
    count = payload[4]
    status = payload[5]
    entries = []
    off = 6
    for _ in range(count):
        if off + 10 > len(payload):
            break
        entries.append({
            "ieee_addr": fmt_ieee(payload[off:off + 8]),
            "nwk_addr": f"0x{be16(payload, off + 8):04x}",
        })
        off += 10
    return {
        "kind": "joined_nodes_rsp",
        "total_count": total,
        "start_index": start,
        "list_count": count,
        "status": status,
        "entries": entries,
    }



def decode_data_confirm(payload: bytes) -> Optional[dict]:
    """Decode ZBHCI_CMD_DATA_CONFIRM (0x8200) conservatively.

    The exact destination-address representation depends on dstAddrMode.
    This decoder keeps unknown/trailing bytes visible instead of guessing.
    """
    if not payload:
        return {"kind": "data_confirm", "raw": "", "truncated": True}

    mode = payload[0]
    off = 1
    result = {"kind": "data_confirm", "dst_addr_mode": mode}

    if mode == 0:
        addr_len, has_dst_ep = 0, False
    elif mode == 1:
        addr_len, has_dst_ep = 2, False
    elif mode == 2:
        addr_len, has_dst_ep = 2, True
    elif mode == 3:
        addr_len, has_dst_ep = 8, True
    else:
        return {
            "kind": "data_confirm",
            "dst_addr_mode": mode,
            "raw": payload.hex(),
            "note": "unknown destination address mode",
        }

    if off + addr_len + 1 > len(payload):
        result.update({"raw": payload.hex(), "truncated": True})
        return result

    if addr_len == 2:
        result["dst_addr"] = f"0x{be16(payload, off):04x}"
    elif addr_len == 8:
        result["dst_addr"] = fmt_ieee(payload[off:off + 8])
    off += addr_len

    result["src_ep"] = payload[off]
    off += 1

    if has_dst_ep:
        if off >= len(payload):
            result.update({"raw": payload.hex(), "truncated": True})
            return result
        result["dst_ep"] = payload[off]
        off += 1

    if off + 4 > len(payload):
        result.update({"raw": payload.hex(), "truncated": True})
        return result

    result["cluster_id"] = f"0x{be16(payload, off):04x}"
    off += 2
    result["status"] = payload[off]
    result["aps_counter"] = payload[off + 1]
    off += 2

    if off < len(payload):
        result["trailing"] = payload[off:].hex()
    return result


def zcl_type_length(data_type: int, payload: bytes) -> Optional[int]:
    fixed = {
        0x08: 1, 0x09: 2, 0x0a: 3, 0x0b: 4,
        0x10: 1, 0x18: 1, 0x20: 1, 0x21: 2, 0x22: 3,
        0x23: 4, 0x24: 5, 0x25: 6, 0x26: 7, 0x27: 8,
        0x28: 1, 0x29: 2, 0x2a: 3, 0x2b: 4, 0x2c: 5,
        0x2d: 6, 0x2e: 7, 0x2f: 8, 0x30: 1, 0x38: 1,
        0x39: 2, 0x3a: 3, 0x3b: 4, 0x3c: 5, 0x3d: 6,
        0x3e: 7, 0x3f: 8,
    }
    if data_type in fixed:
        return fixed[data_type]
    if data_type in (0x41, 0x42, 0x43, 0x44, 0x48):
        return 1 + payload[0] if payload else None
    return None


def decode_zcl_attr_read(payload: bytes) -> Optional[dict]:
    if len(payload) < 8:
        return None
    src_addr = be16(payload, 0)
    src_ep, dst_ep, seq = payload[2:5]
    cluster_id = be16(payload, 5)
    count = payload[7]
    off = 8
    attrs = []
    for _ in range(count):
        if off + 3 > len(payload):
            break
        attr_id = be16(payload, off)
        status = payload[off + 2]
        off += 3
        item = {"attr_id": f"0x{attr_id:04x}", "status": status}
        if status == 0 and off < len(payload):
            data_type = payload[off]
            off += 1
            data_len = zcl_type_length(data_type, payload[off:])
            item["data_type"] = f"0x{data_type:02x}"
            if data_len is None or off + data_len > len(payload):
                item["data_raw"] = payload[off:].hex()
                off = len(payload)
            else:
                value = payload[off:off + data_len]
                item["data_raw"] = value.hex()
                if data_type == 0x10 and value:
                    item["value"] = bool(value[0])
                elif data_type in (0x20, 0x30, 0x38) and value:
                    item["value"] = value[0]
                elif data_type == 0x42 and value:
                    item["value"] = value[1:].decode("utf-8", "replace")
                off += data_len
        attrs.append(item)
    return {
        "kind": "zcl_attr_read_rsp",
        "src_addr": f"0x{src_addr:04x}",
        "src_ep": src_ep,
        "dst_ep": dst_ep,
        "seq": seq,
        "cluster_id": f"0x{cluster_id:04x}",
        "attributes": attrs,
    }


def decode_zcl_read_attr_rsp(payload: bytes) -> list[dict]:
    attrs = []
    off = 0
    while off + 3 <= len(payload):
        attr_id = int.from_bytes(payload[off:off + 2], "little")
        status = payload[off + 2]
        off += 3
        item = {"attr_id": f"0x{attr_id:04x}", "status": status}
        if status != 0:
            attrs.append(item)
            continue
        if off >= len(payload):
            item["error"] = "missing_datatype"
            attrs.append(item)
            break
        data_type = payload[off]
        off += 1
        item["data_type"] = f"0x{data_type:02x}"
        if data_type == 0x10:
            if off >= len(payload):
                item["error"] = "missing_boolean"
                attrs.append(item)
                break
            item["value"] = bool(payload[off])
            item["data_raw"] = payload[off:off + 1].hex()
            off += 1
        elif data_type in (0x20, 0x30, 0x38):
            if off >= len(payload):
                item["error"] = "missing_uint8"
                attrs.append(item)
                break
            item["value"] = payload[off]
            item["data_raw"] = payload[off:off + 1].hex()
            off += 1
        elif data_type in (0x21, 0x29):
            if off + 2 > len(payload):
                item["error"] = "missing_uint16"
                attrs.append(item)
                break
            item["value"] = int.from_bytes(payload[off:off + 2], "little", signed=data_type == 0x29)
            item["data_raw"] = payload[off:off + 2].hex()
            off += 2
        elif data_type in (0x23,):
            if off + 4 > len(payload):
                item["error"] = "missing_uint32"
                attrs.append(item)
                break
            item["value"] = int.from_bytes(payload[off:off + 4], "little")
            item["data_raw"] = payload[off:off + 4].hex()
            off += 4
        elif data_type in (0x41, 0x42):
            if off >= len(payload):
                item["error"] = "missing_string_length"
                attrs.append(item)
                break
            length = payload[off]
            off += 1
            if length == 0xff:
                item["value"] = None
                item["data_raw"] = "ff"
            elif off + length <= len(payload):
                value = payload[off:off + length]
                item["data_raw"] = value.hex()
                item["value"] = value.decode("utf-8", "replace") if data_type == 0x42 else value.hex()
                off += length
            else:
                item["error"] = "truncated_string"
                item["data_raw"] = payload[off:].hex()
                off = len(payload)
        else:
            item["unsupported_type"] = True
            item["raw_remaining"] = payload[off:].hex()
            attrs.append(item)
            break
        attrs.append(item)
    return attrs


def decode_zcl(message: bytes) -> dict:
    if len(message) < 3:
        raise ValueError("ZCL frame too short")
    fc = message[0]
    off = 1
    manufacturer_code = None
    manufacturer_specific = bool(fc & 0x04)
    if manufacturer_specific:
        if off + 2 > len(message):
            raise ValueError("truncated manufacturer code")
        manufacturer_code = int.from_bytes(message[off:off + 2], "little")
        off += 2
    if off + 2 > len(message):
        raise ValueError("truncated ZCL header")
    seq, command_id = message[off:off + 2]
    off += 2
    result = {
        "frame_control": f"0x{fc:02x}",
        "frame_type": fc & 0x03,
        "manufacturer_specific": manufacturer_specific,
        "manufacturer_code": f"0x{manufacturer_code:04x}" if manufacturer_code is not None else None,
        "direction": (fc >> 3) & 1,
        "disable_default_response": bool(fc & 0x10),
        "sequence": seq,
        "command_id": f"0x{command_id:02x}",
        "payload": message[off:].hex(),
    }
    if (fc & 0x03) == 0 and command_id == 0x01:
        result["command"] = "read_attributes_response"
        result["attributes"] = decode_zcl_read_attr_rsp(message[off:])
    return result


def decode_msg_recv(payload: bytes) -> dict:
    if len(payload) < 16:
        return {"kind": "msg_recv", "truncated": True, "raw": payload.hex()}
    src_mode = payload[0]
    src_addr = be16(payload, 1)
    dst_mode = payload[3]
    dst_addr = be16(payload, 4)
    src_ep, dst_ep = payload[6:8]
    profile_id = be16(payload, 8)
    cluster_id = be16(payload, 10)
    message_len = be16(payload, 12)
    expected = 16 + message_len
    if len(payload) != expected:
        return {
            "kind": "msg_recv",
            "error": "bad_length",
            "length": len(payload),
            "expected": expected,
            "message_length": message_len,
            "raw": payload.hex(),
        }
    message = payload[14:14 + message_len]
    result = {
        "kind": "msg_recv",
        "src_addr_mode": src_mode,
        "src_addr": f"0x{src_addr:04x}",
        "dst_addr_mode": dst_mode,
        "dst_addr": f"0x{dst_addr:04x}",
        "src_ep": src_ep,
        "dst_ep": dst_ep,
        "profile_id": f"0x{profile_id:04x}",
        "cluster_id": f"0x{cluster_id:04x}",
        "message_length": message_len,
        "message": message.hex(),
        "meta": payload[14 + message_len:].hex(),
    }
    try:
        result["zcl"] = decode_zcl(message)
    except Exception as exc:
        result["zcl_error"] = str(exc)
    return result


def decode_msg_send(payload: bytes) -> dict:
    result = {
        "kind": "msg_send_rsp",
        "raw": payload.hex(),
        "length": len(payload),
    }
    if len(payload) >= 1:
        result["status"] = payload[0]
    if len(payload) >= 2:
        result["aps_counter"] = payload[1]
    return result


def decode_frame(frame: Frame) -> Optional[dict]:
    t, p = frame.msg_type, frame.payload
    if t == ACK:
        return decode_ack(p)
    if t == 0x8045:
        return decode_netinfo(p)
    if t == 0x8040:
        return decode_joined(p)
    if t == 0x8015:
        return decode_active_ep(p)
    if t == 0x8013:
        return decode_simple_desc(p)
    if t == 0x8012:
        return decode_node_desc(p)
    if t == 0x8030:
        return decode_lqi(p)
    if t == 0x8034:
        return decode_zdo_rsp(p, "permit_join_rsp")
    if t == 0x8200:
        return decode_data_confirm(p)
    if t == ZCL_ATTR_READ_RSP:
        return decode_zcl_attr_read(p)
    if t == 0x8902:
        return decode_msg_recv(p)
    if t == APS_SEND_RSP:
        return decode_msg_send(p)
    if t == 0x8043 and len(p) >= 11:
        return {
            "kind": "device_announce",
            "nwk_addr": f"0x{be16(p, 0):04x}",
            "ieee_addr": fmt_ieee(p[2:10]),
            "capability": f"0x{p[10]:02x}",
        }
    if t == VENDOR_BOOT_IEEE_IND and len(p) == 8:
        return {"kind": "vendor_boot_ieee", "ieee_addr": fmt_ieee(p)}
    if t == 0x8900:
        return {"kind": "vendor_version_rsp", "raw": p.hex()}
    return None


def frames_json(frames: Iterable[Frame]) -> str:
    return json.dumps([f.as_dict() for f in frames], indent=2)


class ZBHCIHost:
    def __init__(self, port: str, baud: int):
        if serial is None:
            raise SystemExit("pyserial not installed: apt install python3-serial")
        self.ser = serial.Serial(
            port,
            baudrate=baud,
            bytesize=8,
            parity="N",
            stopbits=1,
            timeout=0.05,
            rtscts=False,
            xonxoff=False,
        )
        self.parser = FrameParser()
        self.lock = threading.Lock()
        self.unsolicited: list[Frame] = []

    def close(self) -> None:
        self.ser.close()

    def collect(self, duration: float = 0.3) -> list[Frame]:
        deadline = time.monotonic() + duration
        out: list[Frame] = []
        while time.monotonic() < deadline:
            data = self.ser.read(512)
            if data:
                out.extend(self.parser.feed(data))
                # Keep reading briefly after a burst.
                deadline = max(deadline, time.monotonic() + 0.05)
            else:
                time.sleep(0.01)
        return out

    def reset(self) -> list[Frame]:
        with self.lock:
            # Clear only stale bytes BEFORE toggling reset, so boot indications are kept.
            self.ser.reset_input_buffer()
            self.parser = FrameParser()
            power_on()
            return self.collect(0.8)

    def request(
        self,
        msg_type: int,
        payload: bytes = b"",
        wait: float = 1.5,
        expected_type: Optional[int] = None,
    ) -> list[Frame]:
        if expected_type is None:
            expected_type = msg_type | 0x8000

        with self.lock:
            # Preserve any asynchronous indications instead of discarding RX.
            pre = self.collect(0.03)
            if pre:
                self.unsolicited.extend(pre)

            self.ser.write(build_frame(msg_type, payload))
            self.ser.flush()

            deadline = time.monotonic() + wait
            out: list[Frame] = []
            saw_expected = False
            while time.monotonic() < deadline:
                data = self.ser.read(512)
                if data:
                    new = self.parser.feed(data)
                    out.extend(new)
                    if any(f.msg_type == expected_type for f in new):
                        saw_expected = True
                        # ACK and RSP may arrive back-to-back in either order.
                        deadline = min(deadline, time.monotonic() + 0.15)
                else:
                    if saw_expected:
                        time.sleep(0.01)
                    else:
                        time.sleep(0.02)
            return out

    def raw_frame(self, frame_bytes: bytes, wait: float = 1.5) -> list[Frame]:
        with self.lock:
            pre = self.collect(0.03)
            if pre:
                self.unsolicited.extend(pre)
            self.ser.write(frame_bytes)
            self.ser.flush()
            return self.collect(wait)

    # Standard commands -------------------------------------------------
    def set_channel(self, channel: int) -> list[Frame]:
        if not 11 <= channel <= 26:
            raise ValueError("Zigbee channel must be 11..26")
        return self.request(
            BDB_CHANNEL_SET_REQ,
            bytes([channel]),
            wait=1.0,
            expected_type=ACK,
        )

    def formation(self) -> list[Frame]:
        # Public Telink ZBHCI BDB formation command has no payload.
        # Verify completion by polling GET_LOCAL_NWK_INFO (0x0045).
        return self.request(BDB_FORMATION_REQ, b"", wait=1.0, expected_type=ACK)

    def wait_network(self, timeout: float = 15.0, poll: float = 0.5) -> dict:
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            frames = self.netinfo()
            d = first_decoded(frames, "local_network_info")
            if d:
                last = d
                if d.get("on_network"):
                    return {"formed": True, "network": d}
            time.sleep(poll)
        return {"formed": False, "network": last}

    def listen(self, seconds: float) -> list[Frame]:
        if seconds < 0:
            raise ValueError("listen duration must be >= 0")
        with self.lock:
            return self.collect(seconds)

    def netinfo(self) -> list[Frame]:
        return self.request(GET_LOCAL_NWK_INFO_REQ)

    def joined(self, start_idx: int = 0) -> list[Frame]:
        return self.request(NODES_JOINED_GET_REQ, u16(start_idx))

    def node_desc(self, nwk: int) -> list[Frame]:
        # dstAddr + nwkAddrOfInterest
        return self.request(NODE_DESC_REQ, u16(nwk) + u16(nwk), wait=2.0)

    def active_ep(self, nwk: int) -> list[Frame]:
        # dstAddr + nwkAddrOfInterest
        return self.request(ACTIVE_EP_REQ, u16(nwk) + u16(nwk), wait=2.0)

    def simple_desc(self, nwk: int, ep: int) -> list[Frame]:
        if not 0 <= ep <= 0xFF:
            raise ValueError("endpoint must fit in one byte")
        return self.request(SIMPLE_DESC_REQ, u16(nwk) + u16(nwk) + bytes([ep]), wait=2.0)

    def lqi(self, dst: int = 0x0000, start_idx: int = 0) -> list[Frame]:
        if not 0 <= start_idx <= 0xFF:
            raise ValueError("LQI start index must fit in one byte")
        return self.request(MGMT_LQI_REQ, u16(dst) + bytes([start_idx]), wait=2.0)

    def permit_join(self, duration: int, dst: int = 0x0000) -> list[Frame]:
        if not 0 <= duration <= 0xFF:
            raise ValueError("duration must be 0..255")
        # dstAddr + permitDuration + TC_significance(1)
        return self.request(MGMT_PERMIT_JOIN_REQ, u16(dst) + bytes([duration, 1]), wait=2.0)

    def zcl_addr_payload(self, nwk: int, ep: int) -> bytes:
        if not 0 <= ep <= 0xFF:
            raise ValueError("endpoint must fit in one byte")
        return bytes([2]) + u16(nwk) + bytes([0, ep])

    def read_attr(self, nwk: int, ep: int, cluster: int, attr: int) -> list[Frame]:
        payload = bytes([2]) + u16(nwk) + bytes([1, ep])
        payload += u16(0x0104) + bytes([0]) + u16(cluster) + bytes([1]) + u16(attr)
        return self.request(ZCL_ATTR_READ_REQ, payload, wait=2.0)

    def onoff(self, command: int, nwk: int, ep: int) -> list[Frame]:
        return self.request(command, self.zcl_addr_payload(nwk, ep), wait=2.0)

    def join_watch(self, duration: int) -> list[Frame]:
        if not 1 <= duration <= 255:
            raise ValueError("join-watch duration must be 1..255")
        self.permit_join(duration)
        frames = self.listen(duration + 5)
        frames.extend(self.permit_join(0))
        return frames

    def aps_send(self, nwk: int, src_ep: int, dst_ep: int, profile: int,
                 cluster: int, message: bytes) -> list[Frame]:
        if not 0 <= src_ep <= 0xff or not 0 <= dst_ep <= 0xff:
            raise ValueError("endpoint must fit in one byte")
        if len(message) > 0xffff:
            raise ValueError("APS message too large")
        payload = bytes([2]) + u16(nwk) + bytes([src_ep, dst_ep])
        payload += u16(profile) + u16(cluster) + u16(len(message)) + message
        return self.request(APS_SEND_REQ, payload, wait=2.0)

    def aps_read_attr(self, nwk: int, ep: int, cluster: int, attr: int,
                      seq: int = 1) -> list[Frame]:
        message = bytes([0x00, seq & 0xff, 0x00]) + struct.pack("<H", attr)
        return self.aps_send(nwk, 1, ep, 0x0104, cluster, message)

    # Observed vendor extensions ---------------------------------------
    def vendor_version(self) -> list[Frame]:
        return self.request(VENDOR_VERSION_REQ)

    def vendor_0908(self) -> list[Frame]:
        return self.request(VENDOR_UNKNOWN_0908_REQ)


def first_decoded(frames: Iterable[Frame], kind: str) -> Optional[dict]:
    for f in frames:
        d = decode_frame(f)
        if d and d.get("kind") == kind:
            return d
    return None


def selftest(host: ZBHCIHost) -> None:
    print("# reset / boot indications")
    boot = host.reset()
    print(frames_json(boot))

    tests = [
        ("netinfo", host.netinfo),
        ("joined[0]", lambda: host.joined(0)),
        ("node_desc coordinator", lambda: host.node_desc(0x0000)),
        ("active_ep coordinator", lambda: host.active_ep(0x0000)),
        ("mgmt_lqi coordinator", lambda: host.lqi(0x0000, 0)),
    ]

    active_eps: list[int] = []
    for name, fn in tests:
        print(f"\n# {name}")
        frames = fn()
        print(frames_json(frames))
        if name.startswith("active_ep"):
            d = first_decoded(frames, "active_ep_rsp")
            if d and d.get("status") == 0:
                active_eps = d.get("endpoints", [])

    if active_eps:
        ep = active_eps[0]
        print(f"\n# simple_desc coordinator ep={ep}")
        print(frames_json(host.simple_desc(0x0000, ep)))

    # Useful comparison with the current Yandex firmware, but explicitly vendor.
    print("\n# vendor version 0x0900 (optional extension)")
    print(frames_json(host.vendor_version()))

    if host.unsolicited:
        print("\n# asynchronous frames seen before requests")
        print(frames_json(host.unsolicited))


def execute_command(host: ZBHCIHost, parts: list[str]) -> list[Frame] | str:
    if not parts:
        raise ValueError("empty command")
    cmd = parts[0].replace("-", "_")

    if cmd == "reset":
        return host.reset()
    if cmd == "set_channel":
        return host.set_channel(parse_int(parts[1]))
    if cmd == "formation":
        return host.formation()
    if cmd == "wait_network":
        timeout = float(parts[1]) if len(parts) > 1 else 15.0
        return json.dumps(host.wait_network(timeout), indent=2)
    if cmd == "listen":
        seconds = float(parts[1]) if len(parts) > 1 else 5.0
        return host.listen(seconds)
    if cmd == "netinfo":
        return host.netinfo()
    if cmd == "joined":
        start = parse_int(parts[1]) if len(parts) > 1 else 0
        return host.joined(start)
    if cmd == "node_desc":
        return host.node_desc(parse_int(parts[1]))
    if cmd == "active_ep":
        return host.active_ep(parse_int(parts[1]))
    if cmd == "simple_desc":
        return host.simple_desc(parse_int(parts[1]), parse_int(parts[2]))
    if cmd == "lqi":
        dst = parse_int(parts[1]) if len(parts) > 1 else 0x0000
        start = parse_int(parts[2]) if len(parts) > 2 else 0
        return host.lqi(dst, start)
    if cmd == "permit_join":
        duration = parse_int(parts[1])
        dst = parse_int(parts[2]) if len(parts) > 2 else 0x0000
        return host.permit_join(duration, dst)
    if cmd == "read_attr":
        return host.read_attr(parse_int(parts[1]), parse_int(parts[2]), parse_int(parts[3]), parse_int(parts[4]))
    if cmd in ("on", "off", "toggle"):
        command = {"on": ZCL_ONOFF_ON, "off": ZCL_ONOFF_OFF, "toggle": ZCL_ONOFF_TOGGLE}[cmd]
        return host.onoff(command, parse_int(parts[1]), parse_int(parts[2]))
    if cmd == "join_watch":
        return host.join_watch(parse_int(parts[1]))
    if cmd == "aps_send":
        return host.aps_send(
            parse_int(parts[1]), parse_int(parts[2]), parse_int(parts[3]),
            parse_int(parts[4]), parse_int(parts[5]), bytes.fromhex(parts[6]),
        )
    if cmd == "aps_read_attr":
        return host.aps_read_attr(
            parse_int(parts[1]), parse_int(parts[2]), parse_int(parts[3]),
            parse_int(parts[4]), parse_int(parts[5]) if len(parts) > 5 else 1,
        )
    if cmd == "version":
        return host.vendor_version()
    if cmd == "vendor_0908":
        return host.vendor_0908()
    if cmd == "rawmsg":
        msg_type = parse_int(parts[1])
        payload = bytes.fromhex(parts[2]) if len(parts) > 2 else b""
        return host.request(msg_type, payload)
    if cmd == "rawframe":
        return host.raw_frame(bytes.fromhex(parts[1]))
    if cmd == "events":
        frames = list(host.unsolicited)
        host.unsolicited.clear()
        return frames
    if cmd == "quit":
        return "bye"
    raise ValueError(f"unknown command: {cmd}")


def ctrl_server(host: ZBHCIHost, path: str = CTRL_SOCK) -> None:
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    os.chmod(path, 0o660)
    srv.listen(4)
    print(f"CTRL_SOCK={path}", flush=True)

    while True:
        conn, _ = srv.accept()
        with conn:
            f = conn.makefile("rw")
            f.write("ready\n")
            f.flush()
            for line in f:
                parts = line.strip().split()
                if not parts:
                    continue
                try:
                    result = execute_command(host, parts)
                    if isinstance(result, str):
                        f.write(result + "\n")
                        f.flush()
                        if result == "bye":
                            break
                    else:
                        f.write(frames_json(result) + "\n")
                        f.flush()
                except Exception as exc:
                    f.write(json.dumps({"error": str(exc)}) + "\n")
                    f.flush()


def print_result(frames: list[Frame]) -> None:
    print(frames_json(frames))


def main() -> None:
    ap = argparse.ArgumentParser(description="Telink ZBHCI host for Chiron TLSR9218")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--socket", default=CTRL_SOCK)

    sub = ap.add_subparsers(dest="action")
    sub.add_parser("serve", help="Unix-socket control server (default)")
    sub.add_parser("selftest", help="safe standard-ZBHCI probe; does not form/reset network state")
    sub.add_parser("reset", help="power/reset and print boot indications")

    p = sub.add_parser("set-channel", help="set Zigbee channel 11..26 (state-changing)")
    p.add_argument("channel", type=parse_int)

    sub.add_parser("formation", help="start BDB network formation (STATE-CHANGING)")

    p = sub.add_parser("wait-network", help="poll 0x0045 until on_network=true")
    p.add_argument("--timeout", type=float, default=15.0)

    p = sub.add_parser("listen", help="listen for asynchronous ZBHCI indications")
    p.add_argument("seconds", type=float, nargs="?", default=5.0)

    sub.add_parser("netinfo")

    p = sub.add_parser("joined")
    p.add_argument("--start", type=parse_int, default=0)

    p = sub.add_parser("node-desc")
    p.add_argument("nwk", type=parse_int)

    p = sub.add_parser("active-ep")
    p.add_argument("nwk", type=parse_int)

    p = sub.add_parser("simple-desc")
    p.add_argument("nwk", type=parse_int)
    p.add_argument("ep", type=parse_int)

    p = sub.add_parser("lqi")
    p.add_argument("--dst", type=parse_int, default=0x0000)
    p.add_argument("--start", type=parse_int, default=0)

    p = sub.add_parser("permit-join")
    p.add_argument("duration", type=parse_int, help="seconds; 0 disables, 0xff unlimited")
    p.add_argument("--dst", type=parse_int, default=0x0000)

    p = sub.add_parser("read-attr")
    p.add_argument("nwk", type=parse_int)
    p.add_argument("ep", type=parse_int)
    p.add_argument("cluster", type=parse_int)
    p.add_argument("attr", type=parse_int)

    for name in ("on", "off", "toggle"):
        p = sub.add_parser(name)
        p.add_argument("nwk", type=parse_int)
        p.add_argument("ep", type=parse_int)

    p = sub.add_parser("join-watch")
    p.add_argument("seconds", type=parse_int)

    p = sub.add_parser("aps-send")
    p.add_argument("nwk", type=parse_int)
    p.add_argument("src_ep", type=parse_int)
    p.add_argument("dst_ep", type=parse_int)
    p.add_argument("profile", type=parse_int)
    p.add_argument("cluster", type=parse_int)
    p.add_argument("message")

    p = sub.add_parser("aps-read-attr")
    p.add_argument("nwk", type=parse_int)
    p.add_argument("ep", type=parse_int)
    p.add_argument("cluster", type=parse_int)
    p.add_argument("attr", type=parse_int)
    p.add_argument("--seq", type=parse_int, default=1)

    sub.add_parser("version", help="observed vendor extension 0x0900")
    sub.add_parser("vendor-0908", help="observed but not identified vendor extension 0x0908")

    p = sub.add_parser("rawmsg", help="send ZBHCI message type + payload")
    p.add_argument("msg_type", type=parse_int)
    p.add_argument("payload", nargs="?", default="")

    p = sub.add_parser("rawframe", help="send exact raw bytes")
    p.add_argument("hex")

    args = ap.parse_args()
    host = ZBHCIHost(args.port, args.baud)

    try:
        action = args.action or "serve"
        if action == "serve":
            boot = host.reset()
            if boot:
                print("BOOT_FRAMES=" + json.dumps([f.as_dict() for f in boot]), flush=True)
            ctrl_server(host, args.socket)
        elif action == "selftest":
            selftest(host)
        elif action == "reset":
            print_result(host.reset())
        elif action == "set-channel":
            print_result(host.set_channel(args.channel))
        elif action == "formation":
            print_result(host.formation())
        elif action == "wait-network":
            print(json.dumps(host.wait_network(args.timeout), indent=2))
        elif action == "listen":
            print_result(host.listen(args.seconds))
        elif action == "netinfo":
            print_result(host.netinfo())
        elif action == "joined":
            print_result(host.joined(args.start))
        elif action == "node-desc":
            print_result(host.node_desc(args.nwk))
        elif action == "active-ep":
            print_result(host.active_ep(args.nwk))
        elif action == "simple-desc":
            print_result(host.simple_desc(args.nwk, args.ep))
        elif action == "lqi":
            print_result(host.lqi(args.dst, args.start))
        elif action == "permit-join":
            print_result(host.permit_join(args.duration, args.dst))
        elif action == "read-attr":
            print_result(host.read_attr(args.nwk, args.ep, args.cluster, args.attr))
        elif action in ("on", "off", "toggle"):
            command = {"on": ZCL_ONOFF_ON, "off": ZCL_ONOFF_OFF, "toggle": ZCL_ONOFF_TOGGLE}[action]
            print_result(host.onoff(command, args.nwk, args.ep))
        elif action == "join-watch":
            print_result(host.join_watch(args.seconds))
        elif action == "aps-send":
            print_result(host.aps_send(args.nwk, args.src_ep, args.dst_ep,
                                       args.profile, args.cluster,
                                       bytes.fromhex(args.message)))
        elif action == "aps-read-attr":
            print_result(host.aps_read_attr(args.nwk, args.ep, args.cluster,
                                            args.attr, args.seq))
        elif action == "version":
            print_result(host.vendor_version())
        elif action == "vendor-0908":
            print_result(host.vendor_0908())
        elif action == "rawmsg":
            print_result(host.request(args.msg_type, bytes.fromhex(args.payload)))
        elif action == "rawframe":
            print_result(host.raw_frame(bytes.fromhex(args.hex)))
        else:
            ap.error(f"unknown action: {action}")
    finally:
        host.close()


if __name__ == "__main__":
    main()
