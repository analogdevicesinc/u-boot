#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright 2026 Analog Devices Inc.
#
"""Build and inspect ADI ADSP OTP buffers for U-Boot's "fuse writebuff".

The buffer format and the rules U-Boot applies are described in
doc/board/adi/adsp-otp.rst. This tool writes buffers from a JSON
description that names the OTP fields; U-Boot checks every buffer against
the part's current OTP content before programming anything, so build with
--dry-run first and run it on the target to see what would be done.

Commands:
    build SPEC.json -o BUF.bin [--dry-run]   write a buffer
    show BUF.bin                             decode and check a buffer
    fields                                   list the OTP fields

Example SPEC.json for an ADSP-SC58x:

    {
      "soc": "sc58x",
      "flags": ["lock", "fsn-copy"],
      "fields": {
        "public_key0": { "file": "pubkey.bin" },
        "pvt_128key0": { "file": "enckey.bin" },
        "secure_emu_key": [ 0, 17, 34, 51, 68, 85, 102, 119,
                            136, 153, 170, 187, 204, 221, 238, 255 ],
        "gp1": { "offset": 14, "data": [ "0xbe", "0xef" ] },
        "boot_info": { "bcfg": { "pubkey1Inv": 1 } },
        "bootModeDisable": [ "uart" ],
        "antiroll_nv_cntr": 1
      }
    }

Data is always an array of bytes, in OTP address order: the first byte
goes to the lowest address. Each byte is an integer (0-255) or a "0x.."
string. Data must start at, and be a whole number of, the SoC's write
units (2 bytes on the ADSP-SC58x, which programs 16-bit half-words).

Field values:
  - an array of bytes: written from the start of the field (may be
    shorter than the field);
  - {"offset": N, "data": [bytes]}: bytes at byte offset N within the
    field;
  - {"file": "path"} (optionally with "offset"): the bytes of a binary
    file, in file order. The path is relative to SPEC.json. Key fields take
    the files adi_signtool uses, as they are, and check them:
      public_key0/1: the public key from "adi_signtool getkey -type BLKey"
        (56 bytes) or "-type BLKeyC" (C header), ECDSA-224; it must be a
        point on the secp224r1 curve;
      pvt_128key0-3: an AES-128 key file as given to adi_signtool's
        -enckey (BLx) or -wrapkey (BLw), 16 bytes.
    Their bytes go into the OTP unchanged: the boot ROM reads both keys in
    the order adi_signtool writes them;
  - secure_emu_key: also {"userkey": [USERKEY0, USERKEY1, USERKEY2,
    USERKEY3]}, the four 32-bit values a debugger presents to unlock the
    part (OpenOCD: "set USERKEY0_Proc0 0x..."). The boot ROM loads OTP word
    n into TAPC_SDBGKEYn, so USERKEYn goes to 0x1c8 + 4n, little-endian;
  - boot_info: also an object of named sub-fields ("cgu", "bcmd", "bcfg",
    "val14"), each an integer; the tool packs them into the field's bytes
    and unnamed sub-fields stay unrequested;
  - bootModeDisable: an integer, or an array of boot mode names;
  - counters (antiroll_nv_cntr): the target count, an integer.
Zero bytes are never programmed, so fields can be left out or partly
filled. A top-level "raw" array of {"offset": N, "data": [bytes]} objects
writes bytes at absolute OTP offsets.
"""

import argparse
import json
import struct
import sys
import zlib

MAGIC = 0x50544F41  # "AOTP"
VERSION = 1
HDR_FMT = "<IHHIIIIII"
HDR_SIZE = struct.calcsize(HDR_FMT)
ENTRY_FMT = "<HHII"
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)
MAX_SIZE = 0x10000

ENTRY_DATA = 1
ENTRY_COUNTER = 2

CORE_FLAGS = {"lock": 1 << 0, "dry-run": 1 << 1}


class SocSC58x:
    """ADSP-SC58x OTP layout (drivers/misc/adi-otp-sc58x.c)"""

    name = "sc58x"
    soc_id = 0x38354353  # "SC58"
    size = 0x390
    write_size = 2  # 16-bit half-words, each with its own ECC
    flags = {"fsn-copy": 1 << 16, "fsn-ignore": 1 << 17,
             "emu-key-blank": 1 << 18, "boot-mode-unchecked": 1 << 19,
             "cgu-unchecked": 1 << 20, "dmc-unchecked": 1 << 21}
    exclusive = [("fsn-copy", "fsn-ignore")]

    # name: (offset, bytes, kind); kind: data, value, counter, readonly
    fields = {
        "huk": (0x000, 32, "data"),
        "DTCP_key_ecc": (0x020, 160, "data"),
        "DTCP_key_cont": (0x0C0, 40, "data"),
        "DTCP_key_dev": (0x0E8, 128, "data"),
        "pvt_128key0": (0x168, 16, "data"),
        "pvt_128key1": (0x178, 16, "data"),
        "pvt_128key2": (0x188, 16, "data"),
        "pvt_128key3": (0x198, 16, "data"),
        "ek": (0x1A8, 32, "data"),
        "secure_emu_key": (0x1C8, 16, "data"),
        "public_key0": (0x200, 64, "data"),
        "public_key1": (0x240, 64, "data"),
        "boot_info": (0x280, 64, "data"),
        "antiroll_nv_cntr": (0x2C0, 64, "counter"),
        "gp1": (0x300, 64, "data"),
        "bootModeDisable": (0x340, 4, "value"),
        "preboot_ddr_cfg": (0x344, 48, "data"),
        "stageID": (0x374, 8, "data"),
        "fsn": (0x380, 16, "readonly"),
    }
    counter_max = 32

    # The debug key: OpenOCD's USERKEY0-3, TAPC_SDBGKEY0-3
    userkey_field = "secure_emu_key"

    # Fields that hold adi_signtool keys, and the key file format
    key_files = {
        "public_key0": "ecdsa224-public",
        "public_key1": "ecdsa224-public",
        "pvt_128key0": "aes128",
        "pvt_128key1": "aes128",
        "pvt_128key2": "aes128",
        "pvt_128key3": "aes128",
    }

    # Bytes U-Boot never writes: (offset, bytes, why)
    guards = [
        (0x2BC, 4, "boot_info word 15 holds otpTiming, the OTP read timing override"),
        (0x37A, 2, "reserved half-word after stageID"),
    ]

    # bootModeDisable bits (HRM "Boot Mode Disable"): bit n-1 = SYS_BMODE n
    boot_modes = {"spi-master": 0, "spi-slave": 1, "linkport": 5, "uart": 6}

    # boot_info sub-fields (ADI_ROM_OTP_BOOT_INFO, cdefSC58x_rom.h), as
    # bit-fields of the structure's 32-bit words: group -> name ->
    # (word, bit, width). The tool packs them and stores the words
    # little-endian, i.e. as the bytes the OTP holds.
    boot_info = {
        "cgu": {
            "ctl_WEN": (0, 0, 1), "div_WEN": (0, 1, 1),
            "div_DSEL": (0, 3, 5), "div_CSEL": (0, 8, 5),
            "div_S0SEL": (0, 13, 3), "div_SYSSEL": (0, 16, 5),
            "div_S1SEL": (0, 21, 3), "div_OSEL": (0, 24, 7),
            "ctl_DF": (0, 31, 1),
            "ctl_MSEL": (1, 0, 7), "auto_disable": (1, 7, 1),
            "clkoutsel_USBCLKSEL": (1, 8, 6),
            "clkoutsel_CLKOUTSEL": (1, 14, 5), "clkoutsel_WEN": (1, 19, 1),
            "oscwctl0_WEN": (2, 0, 1), "oscwctl0_HODF": (2, 1, 6),
            "oscwctl0_HODEN": (2, 7, 1), "oscwctl0_CNGEN": (2, 8, 1),
            "oscwctl0_BOUF": (2, 9, 5), "oscwctl0_BOUEN": (2, 14, 1),
            "oscwctl0_FAULTEN": (2, 15, 1), "oscwctl0_MONDIS": (2, 16, 1),
            "oscwctl0_FAULTPINDIS": (2, 17, 1),
            "oscwctl1_WEN": (2, 18, 1), "oscwctl1_HODF": (2, 19, 6),
            "oscwctl1_HODEN": (2, 25, 1), "oscwctl1_CNGEN": (2, 26, 1),
            "oscwctl1_BOUF": (2, 27, 5), "oscwctl1_BOUEN": (3, 0, 1),
            "oscwctl1_FAULTEN": (3, 1, 1), "oscwctl1_MONDIS": (3, 2, 1),
            "oscwctl1_FAULTPINDIS": (3, 3, 1),
        },
        "bcmd": {
            "spiMasterBootCmd": (4, 0, 32), "spiSlaveBootCmd": (5, 0, 32),
            "lpBootCmd": (6, 0, 32), "uartBootCmd": (7, 0, 32),
        },
        "bcfg": {
            "cacheDis": (8, 0, 1), "decryptOnlyEn": (8, 16, 1),
            "cacheDisInv": (9, 0, 1), "decryptOnlyInv": (9, 16, 1),
            "pubkey0Inv": (10, 0, 1), "pubkey1Inv": (10, 16, 1),
            "privkey0Inv": (11, 0, 1), "privkey1Inv": (11, 16, 1),
            "privkey2Inv": (12, 0, 1), "privkey3Inv": (12, 16, 1),
            "dmcEn": (13, 0, 1), "dmcInv": (13, 16, 1),
        },
        "val14": {"val14": (14, 0, 32)},
    }


class SocSandbox:
    """The emulated OTP of U-Boot's sandbox (drivers/misc/adi-otp-sandbox.c),
    for testing"""

    name = "sandbox"
    soc_id = 0x584F4253  # "SBOX"
    size = 0x48
    write_size = 2
    flags = {"copy-serial": 1 << 16}
    exclusive = []
    fields = {
        "key": (0x000, 16, "data"),
        "info": (0x010, 16, "data"),
        "value": (0x020, 4, "value"),
        "cntr": (0x030, 16, "counter"),
        "serial": (0x040, 8, "readonly"),
    }
    counter_max = 8
    userkey_field = None
    key_files = {}
    guards = [(0x01C, 4, "test guard")]
    boot_modes = {}
    boot_info = {}


class SocNoKeys:
    """For raw entries: files are taken as plain bytes"""

    key_files = {}


SOCS = {"sandbox": SocSandbox, "sc58x": SocSC58x, "sc589": SocSC58x, "sc587": SocSC58x,
        "sc584": SocSC58x, "sc583": SocSC58x, "sc582": SocSC58x}


class SpecError(Exception):
    pass


def parse_int(val, what):
    if isinstance(val, bool):
        return int(val)
    if isinstance(val, int):
        return val
    if isinstance(val, str):
        try:
            return int(val, 0)
        except ValueError:
            pass
    raise SpecError(f"{what}: not an integer: {val!r}")


def parse_bytes(val, what):
    """An array of bytes, in OTP address order (lowest address first)"""
    if not isinstance(val, list):
        raise SpecError(f"{what}: data must be an array of bytes, lowest "
                        f"address first (e.g. [\"0x01\", \"0x02\"])")
    out = bytearray()
    for i, byte in enumerate(val):
        byte = parse_int(byte, f"{what}[{i}]")
        if not 0 <= byte <= 0xFF:
            raise SpecError(f"{what}[{i}]: {byte} is not a byte (0-255)")
        out.append(byte)
    return bytes(out)


def check_units(soc, what, offset, data):
    """Data must start at, and be a whole number of, write units"""
    if not data:
        raise SpecError(f"{what}: no data")
    if offset % soc.write_size or len(data) % soc.write_size:
        raise SpecError(f"{what}: {len(data)} bytes at offset 0x{offset:x}: "
                        f"the {soc.name} writes {soc.write_size}-byte units, "
                        f"so the offset and the length must be multiples of "
                        f"{soc.write_size}")


def pack_boot_info(soc, spec):
    words = [0] * 16
    for group, members in spec.items():
        if group not in soc.boot_info:
            raise SpecError(f"boot_info: unknown group {group!r} "
                            f"(one of {', '.join(soc.boot_info)})")
        if group == "val14" and not isinstance(members, dict):
            members = {"val14": members}
        if not isinstance(members, dict):
            raise SpecError(f"boot_info.{group}: expected an object")
        for name, value in members.items():
            layout = soc.boot_info[group].get(name)
            if layout is None:
                raise SpecError(f"boot_info.{group}: unknown field {name!r}")
            word, bit, width = layout
            value = parse_int(value, f"boot_info.{group}.{name}")
            if not 0 <= value < (1 << width):
                raise SpecError(f"boot_info.{group}.{name}: {value} does not fit in {width} bits")
            words[word] |= value << bit
    return struct.pack("<16I", *words)


# secp224r1 (NIST P-224), the curve of adi_signtool's ecdsa224 keys
P224_P = 2**224 - 2**96 + 1
P224_B = 0xB4050A850C04B3ABF54132565044B0B7D7BFD8BA270B39432355FFB4


def read_signtool_public_key(path, name):
    """The 56-byte ECDSA-224 public key Qx || Qy (big-endian numbers), from
    the binary (-type BLKey) or C header (-type BLKeyC) output of
    "adi_signtool getkey", as the boot ROM reads it from OTP."""
    with open(path, "rb") as f:
        raw = f.read()

    if len(raw) == 56:
        key = raw
    elif raw.startswith(b"\x30") and len(raw) > 64:
        raise SpecError(f"{name}: {path} looks like a DER keypair, which holds "
                        f"the private key; extract the public key with "
                        f"'adi_signtool getkey -type BLKey -key {path} "
                        f"-outfile pubkey.bin'")
    elif len(raw) == 64:
        raise SpecError(f"{name}: {path} is 64 bytes, an ECDSA-256 key; "
                        f"the ADSP-SC58x only supports ECDSA-224")
    else:
        try:
            text = raw.decode("ascii")
        except UnicodeDecodeError:
            raise SpecError(f"{name}: {path} is {len(raw)} bytes, not a "
                            f"56-byte ECDSA-224 public key") from None
        key = parse_signtool_header(text, path, name)

    x = int.from_bytes(key[:28], "big")
    y = int.from_bytes(key[28:], "big")
    if not (0 < x < P224_P and 0 < y < P224_P) or \
            (y * y - (x * x * x - 3 * x + P224_B)) % P224_P:
        raise SpecError(f"{name}: {path} is not a secp224r1 public key "
                        f"(wrong file, or bytes reordered)")
    return key


def parse_signtool_header(text, path, name):
    """The bytes of the uint8_t array of an "adi_signtool getkey -type
    BLKeyC" header"""
    import re

    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    match = re.search(r"uint8_t\s+\w+\s*\[\s*\]\s*=\s*\{([^}]*)\}", text)
    if not match:
        raise SpecError(f"{name}: {path} is neither a 56-byte binary public "
                        f"key nor an adi_signtool BLKeyC header")
    values = [v.strip() for v in match.group(1).split(",") if v.strip()]
    try:
        key = bytes(int(v, 0) for v in values)
    except ValueError:
        raise SpecError(f"{name}: {path}: bad byte in the key array") from None
    if len(key) != 56:
        raise SpecError(f"{name}: {path} holds {len(key)} bytes, not a "
                        f"56-byte ECDSA-224 public key")
    return key


def pack_userkey(name, value):
    """The OTP bytes of a debug key given as the four 32-bit values a
    debugger presents (OpenOCD's USERKEY0-3): the boot ROM loads OTP word n
    into TAPC_SDBGKEYn, so each value is stored little-endian, in order."""
    if set(value) != {"userkey"}:
        raise SpecError(f"{name}: expected {{\"userkey\": [4 values]}}")
    words = value["userkey"]
    if not isinstance(words, list) or len(words) != 4:
        raise SpecError(f"{name}: userkey is four 32-bit values, USERKEY0 first")
    key = b""
    for i, word in enumerate(words):
        word = parse_int(word, f"{name}.userkey[{i}]")
        if not 0 <= word <= 0xFFFFFFFF:
            raise SpecError(f"{name}.userkey[{i}]: not a 32-bit value")
        key += struct.pack("<I", word)
    if not any(key):
        raise SpecError(f"{name}: an all-zero debug key is a debugger's "
                        f"default; leave the field out instead")
    if key == b"\xff" * 16:
        print(f"warning: {name} is all ones: the boot ROM loads no debug key, "
              f"so a locked part has no debug access", file=sys.stderr)
    return key


def read_aes128_key(path, name):
    """An AES-128 key file, as adi_signtool's -enckey and -wrapkey take it"""
    with open(path, "rb") as f:
        key = f.read()
    if len(key) != 16:
        raise SpecError(f"{name}: {path} is {len(key)} bytes; an AES-128 "
                        f"key file is 16 bytes")
    if not any(key):
        raise SpecError(f"{name}: {path} is all zeros")
    return key


def read_file(soc, name, path, base):
    """The bytes for a field from a file, checked if it is a key"""
    import os

    path = os.path.join(base, path)
    kind = soc.key_files.get(name)
    try:
        if kind == "ecdsa224-public":
            return read_signtool_public_key(path, name)
        if kind == "aes128":
            return read_aes128_key(path, name)
        with open(path, "rb") as f:
            return f.read()
    except OSError as err:
        raise SpecError(f"{name}: {err}") from None


def field_entries(soc, name, value, base="."):
    """Entries for one named field: [(type, offset, data)]"""
    if name not in soc.fields:
        raise SpecError(f"unknown field {name!r} (see 'fields')")
    offset, size, kind = soc.fields[name]

    if kind == "readonly":
        raise SpecError(f"{name} is read-only")
    if kind == "counter":
        target = parse_int(value, name)
        if not 0 <= target <= soc.counter_max:
            raise SpecError(f"{name}: {target} is not in 0..{soc.counter_max}")
        return [(ENTRY_COUNTER, offset, struct.pack("<I", target))]
    if kind == "value":
        if isinstance(value, list):
            mask = 0
            for mode in value:
                if mode not in soc.boot_modes:
                    raise SpecError(f"{name}: unknown boot mode {mode!r} "
                                    f"(one of {', '.join(soc.boot_modes)})")
                mask |= 1 << soc.boot_modes[mode]
            value = mask
        value = parse_int(value, name)
        if not 0 <= value <= 0xFF:
            raise SpecError(f"{name}: {value} is not an 8-bit value")
        # The value is the field's lowest-addressed byte
        data = bytes([value]) + bytes(soc.write_size - 1)
        return [(ENTRY_DATA, offset, data)]

    rel = 0
    if name == soc.userkey_field and isinstance(value, dict) and \
            "userkey" in value:
        data = pack_userkey(name, value)
    elif name == "boot_info" and isinstance(value, dict) and \
            "data" not in value:
        data = pack_boot_info(soc, value)
    elif isinstance(value, dict):
        keys = set(value)
        if keys - {"offset", "data", "file"} or \
                len(keys & {"data", "file"}) != 1:
            raise SpecError(f"{name}: expected {{\"offset\": N, \"data\": "
                            f"[bytes]}} or {{\"offset\": N, \"file\": \"path\"}}")
        rel = parse_int(value.get("offset", 0), f"{name}.offset")
        if "file" in value:
            if name in soc.key_files and rel:
                raise SpecError(f"{name}: a key file is written from the "
                                f"start of the field, without an offset")
            data = read_file(soc, name, value["file"], base)
        else:
            data = parse_bytes(value["data"], name)
    else:
        data = parse_bytes(value, name)

    check_units(soc, name, rel, data)
    if rel < 0 or rel + len(data) > size:
        raise SpecError(f"{name}: {len(data)} bytes at +{rel} exceed the "
                        f"field's {size} bytes")
    return [(ENTRY_DATA, offset + rel, data)]


def check_guards(soc, entries):
    for etype, offset, data in entries:
        if etype != ENTRY_DATA:
            continue
        for i, byte in enumerate(data):
            if not byte:
                continue
            pos = offset + i
            for goff, glen, why in soc.guards:
                if goff <= pos < goff + glen:
                    raise SpecError(f"0x{pos:03x} is protected: {why}")


def strip_zero_entries(entries):
    """Drop DATA entries that request nothing (all zero)"""
    return [e for e in entries if e[0] != ENTRY_DATA or any(e[2])]


def build(spec, base="."):
    soc_name = str(spec.get("soc", "")).lower()
    soc = SOCS.get(soc_name)
    if soc is None:
        raise SpecError(f"unknown soc {spec.get('soc')!r} "
                        f"(one of {', '.join(sorted(SOCS))})")

    flags = 0
    names = spec.get("flags", [])
    for name in names:
        if name in CORE_FLAGS:
            flags |= CORE_FLAGS[name]
        elif name in soc.flags:
            flags |= soc.flags[name]
        else:
            raise SpecError(f"unknown flag {name!r} (one of "
                            f"{', '.join(list(CORE_FLAGS) + list(soc.flags))})")
    for group in soc.exclusive:
        if sum(name in names for name in group) > 1:
            raise SpecError(f"flags {' and '.join(group)} are mutually exclusive")

    entries = []
    for name, value in spec.get("fields", {}).items():
        entries += field_entries(soc, name, value, base)
    for i, raw in enumerate(spec.get("raw", [])):
        if not isinstance(raw, dict) or "offset" not in raw or \
                set(raw) - {"offset", "data", "file"} or \
                len(set(raw) & {"data", "file"}) != 1:
            raise SpecError(f"raw[{i}]: expected {{\"offset\": N, \"data\": "
                            f"[bytes]}} or {{\"offset\": N, \"file\": \"path\"}}")
        offset = parse_int(raw["offset"], f"raw[{i}].offset")
        if "file" in raw:
            data = read_file(SocNoKeys, f"raw[{i}]", raw["file"], base)
        else:
            data = parse_bytes(raw["data"], f"raw[{i}].data")
        check_units(soc, f"raw[{i}]", offset, data)
        if offset < 0 or offset + len(data) > soc.size:
            raise SpecError(f"raw[{i}]: 0x{offset:x}+{len(data)} is outside "
                            f"the OTP area (0x{soc.size:x} bytes)")
        entries.append((ENTRY_DATA, offset, data))
    unknown = set(spec) - {"soc", "flags", "fields", "raw"}
    if unknown:
        raise SpecError(f"unknown keys: {', '.join(sorted(unknown))}")

    entries = strip_zero_entries(entries)
    check_guards(soc, entries)
    return encode(soc.soc_id, flags, entries)


def encode(soc_id, flags, entries):
    body = b""
    for etype, offset, data in entries:
        pad = (-len(data)) % 4
        body += struct.pack(ENTRY_FMT, etype, 0, offset, len(data))
        body += data + b"\0" * pad
    total = HDR_SIZE + len(body)
    if total > MAX_SIZE:
        raise SpecError(f"buffer too large ({total} bytes)")
    hdr = struct.pack(HDR_FMT, MAGIC, VERSION, HDR_SIZE, soc_id, flags,
                      total, len(entries), 0, 0)
    crc = zlib.crc32(hdr + body) & 0xFFFFFFFF
    hdr = struct.pack(HDR_FMT, MAGIC, VERSION, HDR_SIZE, soc_id, flags,
                      total, len(entries), crc, 0)
    return hdr + body


def decode(buf):
    """Check a buffer and return (soc, flags, entries)"""
    if len(buf) < HDR_SIZE:
        raise SpecError("too short for a header")
    (magic, version, hdr_size, soc_id, flags, total, count, crc,
     reserved) = struct.unpack_from(HDR_FMT, buf)
    if magic != MAGIC:
        raise SpecError("bad magic")
    if version != VERSION or hdr_size != HDR_SIZE or reserved:
        raise SpecError(f"unsupported version {version} / header size {hdr_size}")
    if total < HDR_SIZE or total > MAX_SIZE or total % 4 or total > len(buf):
        raise SpecError(f"bad total size {total}")
    check = bytearray(buf[:total])
    struct.pack_into("<I", check, 24, 0)
    if zlib.crc32(check) & 0xFFFFFFFF != crc:
        raise SpecError("CRC mismatch")
    soc = next((s for s in SOCS.values() if s.soc_id == soc_id), None)
    if soc is None:
        raise SpecError(f"unknown SoC id 0x{soc_id:08x}")

    entries, pos = [], HDR_SIZE
    for i in range(count):
        if total - pos < ENTRY_SIZE:
            raise SpecError(f"entry {i}: truncated")
        etype, res, offset, length = struct.unpack_from(ENTRY_FMT, buf, pos)
        pos += ENTRY_SIZE
        padded = length + (-length) % 4
        if res or total - pos < padded:
            raise SpecError(f"entry {i}: bad entry")
        entries.append((etype, offset, bytes(buf[pos:pos + length])))
        pos += padded
    if pos != total:
        raise SpecError(f"{total - pos} stray bytes after the last entry")
    return soc, flags, entries


def field_name(soc, offset):
    for name, (foff, size, _) in soc.fields.items():
        if foff <= offset < foff + size:
            return f"{name}+0x{offset - foff:x}" if offset != foff else name
    return "reserved"


def flag_names(soc, flags):
    names = [n for n, b in list(CORE_FLAGS.items()) + list(soc.flags.items())
             if flags & b]
    rest = flags & ~sum(list(CORE_FLAGS.values()) + list(soc.flags.values()))
    if rest:
        names.append(f"0x{rest:08x}")
    return ", ".join(names) or "none"


def cmd_build(args):
    import os

    with open(args.spec, encoding="utf-8") as f:
        spec = json.load(f)
    if args.dry_run:
        spec.setdefault("flags", [])
        if "dry-run" not in spec["flags"]:
            spec["flags"].append("dry-run")
    buf = build(spec, os.path.dirname(os.path.abspath(args.spec)))
    with open(args.output, "wb") as f:
        f.write(buf)
    print(f"{args.output}: {len(buf)} bytes")
    soc = SOCS[str(spec.get("soc", "")).lower()]
    value = spec.get("fields", {}).get(soc.userkey_field)
    words = value.get("userkey") if isinstance(value, dict) else None
    if words and [parse_int(w, "userkey") for w in words] != [0xFFFFFFFF] * 4:
        keys = "; ".join(f"set USERKEY{i}_Proc0 0x{parse_int(w, 'userkey'):08x}"
                         for i, w in enumerate(words))
        print(f"OpenOCD, to debug the part once locked (keep it secret): "
              f"-c \"{keys}\"")
    if args.dry_run:
        print("dry run: U-Boot will check it and report, but program nothing")
    print(f"U-Boot: load it to <addr>, then: fuse writebuff <addr>")


def cmd_show(args):
    with open(args.buffer, "rb") as f:
        soc, flags, entries = decode(f.read())
    print(f"SoC {soc.name} (0x{soc.soc_id:08x}), flags: {flag_names(soc, flags)}")
    for i, (etype, offset, data) in enumerate(entries):
        if etype == ENTRY_COUNTER and len(data) == 4:
            print(f"  [{i}] COUNTER {field_name(soc, offset)} -> "
                  f"{struct.unpack('<I', data)[0]}")
        elif etype == ENTRY_DATA:
            nonzero = sum(1 for b in data if b)
            print(f"  [{i}] DATA 0x{offset:03x} {field_name(soc, offset)}: "
                  f"{len(data)} bytes, {nonzero} non-zero")
        else:
            print(f"  [{i}] type {etype} at 0x{offset:03x}: unknown")


def cmd_fields(args):
    soc = SOCS[args.soc]
    print(f"{soc.name}: OTP area 0x000-0x{soc.size - 1:03x}, "
          f"{soc.write_size}-byte write units")
    for name, (offset, size, kind) in soc.fields.items():
        print(f"  0x{offset:03x} {name:17} {size:4} bytes  {kind}")
    print("boot_info sub-fields:")
    for group, members in soc.boot_info.items():
        print(f"  {group}: {', '.join(members)}")
    print(f"bootModeDisable names: {', '.join(soc.boot_modes)}")
    print(f"flags: {', '.join(list(CORE_FLAGS) + list(soc.flags))}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("build", help="write a buffer from a JSON description")
    p.add_argument("spec")
    p.add_argument("-o", "--output", required=True)
    p.add_argument("--dry-run", action="store_true",
                   help="set the dry-run flag: U-Boot only checks and reports")
    p.set_defaults(func=cmd_build)
    p = sub.add_parser("show", help="decode and check a buffer")
    p.add_argument("buffer")
    p.set_defaults(func=cmd_show)
    p = sub.add_parser("fields", help="list the OTP fields")
    p.add_argument("--soc", default="sc58x", choices=sorted(SOCS))
    p.set_defaults(func=cmd_fields)
    args = parser.parse_args(argv)
    try:
        args.func(args)
    except (SpecError, OSError, json.JSONDecodeError) as err:
        print(f"error: {err}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
