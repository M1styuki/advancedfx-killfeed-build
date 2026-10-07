#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Deterministic generator for the authored storm-bolt particle resource.

This emits an *original*, minimal Source 2 particle definition (not a copy of,
or a patch to, any Valve asset) whose only external reference is the installed
game texture ``materials/particle/bendibeam.vtex``.  The definition is written
as a Source 2 resource container holding an uncompressed KeyValues3 v3 DATA
block plus a RERL external-reference block.

Only the standard library is used, and the output is byte-for-byte
deterministic for a given script revision, so CI can regenerate it instead of
checking in an opaque compiled asset.

Run::

    python tools/generate_weather_bolt.py --output <path/storm_bolt.vpcf_c>

The generator re-reads its own bytes with an independent decoder and fails
closed if any pool, alignment, string, resource flag, external-reference hash
or authored property does not match what it intended to write.

Format facts used here (Source 2 resource container + binary KV3 v3) were
established independently; see doc/notes/seven-map-storm-native.md for the
behaviour notes.  No game bytes or decoded game documents are embedded.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

# --------------------------------------------------------------------------
# Constants
# --------------------------------------------------------------------------

#: Binary KV3 version 3 magic ("KV3\x03" little endian).
KV3_MAGIC_V3 = 0x4B563303

#: Format GUID carried by the native particle DATA blocks (generic KV3 format).
KV3_FORMAT_GUID = bytes.fromhex("95156b32e8450440aa5a3e08655ff51f")

#: Trailer that terminates every v3 type stream.
KV3_TRAILER = 0xFFEEDD00

#: External texture: installed game asset, referenced by name only.
BOLT_TEXTURE = "materials/particle/bendibeam.vtex"

#: MurmurHash64B seed used by the engine for resource file names.
RESOURCE_HASH_SEED = 0xEDABCDEF

#: Container header.
CONTAINER_HEADER_VERSION = 12
CONTAINER_RESOURCE_VERSION = 1

# --------------------------------------------------------------------------
# MurmurHash64B (resource file-name hash)
# --------------------------------------------------------------------------


def murmur_hash64b(data: bytes, seed: int = RESOURCE_HASH_SEED) -> int:
    """MurmurHash64B as used by the engine for compiled resource file names."""
    mask = 0xFFFFFFFF
    m = 0x5BD1E995
    h1 = (seed & mask) ^ len(data)
    h2 = seed >> 32
    pos = 0

    def step(k: int) -> int:
        k = (k * m) & mask
        k ^= k >> 24
        return (k * m) & mask

    while len(data) - pos >= 8:
        k1, k2 = struct.unpack_from("<II", data, pos)
        pos += 8
        h1 = ((h1 * m) & mask) ^ step(k1)
        h2 = ((h2 * m) & mask) ^ step(k2)
    if len(data) - pos >= 4:
        (k,) = struct.unpack_from("<I", data, pos)
        pos += 4
        h1 = ((h1 * m) & mask) ^ step(k)
    if pos < len(data):
        h2 = (h2 ^ int.from_bytes(data[pos:], "little")) * m & mask

    h1 = ((h1 ^ (h2 >> 18)) * m) & mask
    h2 = ((h2 ^ (h1 >> 22)) * m) & mask
    h1 = ((h1 ^ (h2 >> 17)) * m) & mask
    h2 = ((h2 ^ (h1 >> 19)) * m) & mask
    return (h1 << 32) | h2


# --------------------------------------------------------------------------
# Tiny typed document model
# --------------------------------------------------------------------------


def kv_int(value: int):
    return ("i", int(value))


def kv_float(value: float):
    # Native particle definitions encode float32 fields as KV3 DOUBLE nodes.
    return ("d", float(value))


def kv_bool(value: bool):
    return ("b", bool(value))


def kv_string(value: str):
    return ("s", str(value))


def kv_resource(value: str):
    """A string that carries the KV3 resource-reference flag."""
    return ("r", str(value))


def kv_object(members: dict):
    return ("o", members)


def kv_array(items):
    return ("a", list(items))


def kv_typed_double_array(values):
    """KV3 ARRAY_TYPED (type 10) of DOUBLE, the native vector short form."""
    return ("t", [float(v) for v in values])


def _float_to_int_bits(value: float) -> int:
    return struct.unpack("<i", struct.pack("<f", value))[0]


# --------------------------------------------------------------------------
# KV3 v3 writer
# --------------------------------------------------------------------------


class Kv3Writer:
    """Serialises the typed document model into an uncompressed KV3 v3 block.

    Layout (matching the engine's reader for versions < 5):

    * byte lane (unused here)
    * 4-aligned 32-bit lane, whose first value is the string count
    * 8-aligned 64-bit lane
    * 8-alignment of the end of the pools
    * NUL-terminated UTF-8 string table
    * pre-order type stream
    * 0xFFEEDD00 trailer
    """

    def __init__(self) -> None:
        self.types = bytearray()
        self.ints: list[int] = []
        self.doubles: list[float] = []
        self.bytes1 = bytearray()
        self.strings: list[str] = []
        self._string_index: dict[str, int] = {}
        self.object_count = 0
        self.array_count = 0

    def _index(self, text: str) -> int:
        index = self._string_index.get(text)
        if index is None:
            index = len(self.strings)
            self._string_index[text] = index
            self.strings.append(text)
        return index

    def _emit_type(self, node) -> None:
        kind = node[0]
        if kind == "o":
            self.types.append(9)
        elif kind == "a":
            self.types.append(8)
        elif kind == "t":
            self.types.append(10)
        elif kind == "f":
            self.types.append(19)
        elif kind == "d":
            self.types.append(5)
        elif kind == "i":
            self.types.append(11)
        elif kind == "b":
            self.types.append(13 if node[1] else 14)
        elif kind == "s":
            self.types.append(6)
        elif kind == "r":
            # 0x80 selects a flag byte, value 1 means "resource reference".
            self.types.append(0x86)
            self.types.append(1)
        else:  # pragma: no cover - internal invariant
            raise ValueError("unknown node kind %r" % (kind,))

    def _emit_payload(self, node) -> None:
        kind = node[0]
        if kind == "o":
            members = node[1]
            self.object_count += 1
            self.ints.append(len(members))
            for name, child in members.items():
                self._emit_type(child)
                self.ints.append(self._index(name))
                self._emit_payload(child)
        elif kind == "a":
            items = node[1]
            self.array_count += 1
            self.ints.append(len(items))
            for child in items:
                self._emit_type(child)
                self._emit_payload(child)
        elif kind == "t":
            items = node[1]
            self.array_count += 1
            self.ints.append(len(items))
            self._emit_type(("d", 0.0))
            for child in items:
                self._emit_payload(("d", child))
        elif kind == "f":
            self.ints.append(_float_to_int_bits(node[1]))
        elif kind == "d":
            self.doubles.append(node[1])
        elif kind == "i":
            self.ints.append(int(node[1]))
        elif kind in ("s", "r"):
            self.ints.append(self._index(node[1]))
        elif kind == "b":
            pass
        else:  # pragma: no cover - internal invariant
            raise ValueError("unknown node kind %r" % (kind,))

    def encode(self, root) -> bytes:
        self._emit_type(root)
        self._emit_payload(root)

        string_region = bytearray()
        for text in self.strings:
            string_region += text.encode("utf-8") + b"\x00"
        type_region = bytes(self.types)
        count_types = len(string_region) + len(type_region)

        int_values = [len(self.strings)] + self.ints
        count1 = len(self.bytes1)
        count4 = len(int_values)
        count8 = len(self.doubles)

        pool = bytearray()
        pool += bytes(self.bytes1)
        while len(pool) % 4:
            pool.append(0)
        for value in int_values:
            pool += struct.pack("<i", value)
        while len(pool) % 8:
            pool.append(0)
        for value in self.doubles:
            pool += struct.pack("<d", value)
        # Empty 8-byte lanes are still aligned before v5.
        while len(pool) % 8:
            pool.append(0)

        pool += bytes(string_region)
        pool += type_region
        pool += struct.pack("<I", KV3_TRAILER)

        size_uncompressed = len(pool)
        header = bytearray()
        header += struct.pack("<I", KV3_MAGIC_V3)
        header += KV3_FORMAT_GUID
        header += struct.pack("<I", 0)  # compression method: uncompressed
        header += struct.pack("<H", 0)  # compression dictionary id
        header += struct.pack("<H", 0)  # compression frame size
        header += struct.pack("<i", count1)
        header += struct.pack("<i", count4)
        header += struct.pack("<i", count8)
        header += struct.pack("<i", count_types)
        header += struct.pack("<H", self.object_count)
        header += struct.pack("<H", self.array_count)
        header += struct.pack("<i", size_uncompressed)
        header += struct.pack("<i", size_uncompressed)  # equal when uncompressed
        header += struct.pack("<i", 0)  # block count
        header += struct.pack("<i", 0)  # binary blob bytes
        if len(header) != 64:  # pragma: no cover - internal invariant
            raise AssertionError("KV3 v3 header must be 64 bytes")

        return bytes(header) + bytes(pool)


# --------------------------------------------------------------------------
# Resource container (RERL + DATA)
# --------------------------------------------------------------------------


def build_rerl(entries) -> bytes:
    """RERL block: uint32 tableOffset, uint32 count, uint32 pad, entries."""
    payload = bytearray()
    payload += struct.pack("<III", 12, len(entries), 0)
    table_start = len(payload)
    names_start = table_start + 16 * len(entries)
    position = names_start
    offsets = []
    for name, _resource_id in entries:
        offsets.append(position - (table_start + 16 * len(offsets) + 8))
        position += len(name.encode("utf-8")) + 1
    for (name, resource_id), name_offset in zip(entries, offsets):
        payload += struct.pack("<Q", resource_id)
        payload += struct.pack("<q", name_offset)
    for name, _resource_id in entries:
        payload += name.encode("utf-8") + b"\x00"
    return bytes(payload)


def build_container(blocks) -> bytes:
    """Assemble the Source 2 resource container.

    ``blocks`` is a sequence of ``(kind, bytes)``.  DATA is 16-byte aligned to
    match the native compiled resources; the block offset field is relative to
    the offset field's own address.
    """
    header_size = 16
    descriptor_size = 12 * len(blocks)
    cursor = header_size + descriptor_size
    layout = []
    for kind, blob in blocks:
        if kind == b"DATA":
            cursor = (cursor + 15) & ~15
        layout.append((kind, blob, cursor))
        cursor += len(blob)

    out = bytearray()
    out += struct.pack("<IHHII", 0, CONTAINER_HEADER_VERSION, CONTAINER_RESOURCE_VERSION, 8, len(blocks))
    for index, (kind, blob, start) in enumerate(layout):
        position = header_size + 12 * index
        relative = start - (position + 4)
        out += kind + struct.pack("<II", relative, len(blob))
    for _kind, blob, start in layout:
        while len(out) < start:
            out.append(0)
        out += blob
    struct.pack_into("<I", out, 0, len(out))
    return bytes(out)


# --------------------------------------------------------------------------
# Authored particle definition
# --------------------------------------------------------------------------


def build_document() -> dict:
    """The authored bolt: 32 rope points from CP0 to CP1, ~0.22 s lifetime."""
    return kv_object(
        {
            "_class": kv_string("CParticleSystemDefinition"),
            "m_nBehaviorVersion": kv_int(4),
            "m_nMaxParticles": kv_int(32),
            # Cool white; particles carry alpha 1 from the initializer below.
            "m_ConstantColor": kv_array([kv_int(238), kv_int(246), kv_int(255), kv_int(255)]),
            "m_Renderers": kv_array(
                [
                    kv_object(
                        {
                            "_class": kv_string("C_OP_RenderRopes"),
                            "m_flOverbrightFactor": kv_float(10.0),
                            "m_flRadiusScale": kv_float(0.65),
                            "m_flTextureVWorldSize": kv_float(128.0),
                            "m_bDisableOperator": kv_bool(False),
                            "m_vecTexturesInput": kv_array(
                                [kv_object({"m_hTexture": kv_resource(BOLT_TEXTURE)})]
                            ),
                            "m_nOutputBlendMode": kv_string("PARTICLE_OUTPUT_BLEND_MODE_ADD"),
                        }
                    )
                ]
            ),
            "m_Initializers": kv_array(
                [
                    kv_object(
                        {
                            "_class": kv_string("C_INIT_CreateSequentialPath"),
                            "m_flNumToAssign": kv_float(32.0),
                            "m_PathParams": kv_object(
                                {
                                    "m_nStartControlPointNumber": kv_int(0),
                                    "m_nEndControlPointNumber": kv_int(1),
                                }
                            ),
                        }
                    ),
                    kv_object(
                        {
                            "_class": kv_string("C_INIT_PositionOffset"),
                            "m_OffsetMin": kv_typed_double_array([-24.0, -24.0, -3.0]),
                            "m_OffsetMax": kv_typed_double_array([24.0, 24.0, 3.0]),
                        }
                    ),
                    kv_object(
                        {
                            "_class": kv_string("C_INIT_RandomRadius"),
                            "m_flRadiusMin": kv_float(4.0),
                            "m_flRadiusMax": kv_float(6.0),
                            "m_flRadiusRandExponent": kv_float(1.0),
                        }
                    ),
                    kv_object(
                        {
                            "_class": kv_string("C_INIT_RandomLifeTime"),
                            "m_flLifetimeMin": kv_float(0.20),
                            "m_flLifetimeMax": kv_float(0.24),
                            "m_flLifetimeRandExponent": kv_float(1.0),
                        }
                    ),
                    kv_object(
                        {
                            "_class": kv_string("C_INIT_RandomAlpha"),
                            "m_nFieldOutput": kv_int(7),
                            "m_nAlphaMin": kv_int(255),
                            "m_nAlphaMax": kv_int(255),
                            "m_flAlphaRandExponent": kv_float(1.0),
                        }
                    ),
                ]
            ),
            "m_Operators": kv_array(
                [
                    kv_object({"_class": kv_string("C_OP_BasicMovement")}),
                    kv_object({"_class": kv_string("C_OP_Decay")}),
                    kv_object(
                        {
                            "_class": kv_string("C_OP_FadeOut"),
                            "m_flFadeOutTimeMin": kv_float(0.05),
                            "m_flFadeOutTimeMax": kv_float(0.08),
                        }
                    ),
                ]
            ),
            "m_Emitters": kv_array(
                [
                    kv_object(
                        {
                            "_class": kv_string("C_OP_InstantaneousEmitter"),
                            "m_nParticlesToEmit": kv_object(
                                {
                                    "m_nType": kv_string("PF_TYPE_LITERAL"),
                                    "m_flLiteralValue": kv_float(32.0),
                                }
                            ),
                        }
                    )
                ]
            ),
        }
    )


def build_resource() -> bytes:
    writer = Kv3Writer()
    data_block = writer.encode(build_document())
    resource_id = murmur_hash64b(BOLT_TEXTURE.encode("utf-8"))
    rerl_block = build_rerl([(BOLT_TEXTURE, resource_id)])
    return build_container([(b"RERL", rerl_block), (b"DATA", data_block)])


# --------------------------------------------------------------------------
# Independent reader / verification
# --------------------------------------------------------------------------


def _read_container(data: bytes):
    (file_size,) = struct.unpack_from("<I", data, 0)
    header_version, resource_version, block_offset, block_count = struct.unpack_from("<HHII", data, 4)
    if file_size != len(data):
        raise ValueError("container size %d != file size %d" % (file_size, len(data)))
    if header_version != CONTAINER_HEADER_VERSION or resource_version != CONTAINER_RESOURCE_VERSION:
        raise ValueError("container version %d/%d" % (header_version, resource_version))
    blocks = {}
    for index in range(block_count):
        position = 8 + block_offset + 12 * index
        kind = bytes(data[position : position + 4])
        relative, size = struct.unpack_from("<II", data, position + 4)
        start = position + 4 + relative
        if not (0 <= start and start + size <= len(data)):
            raise ValueError("block %r out of range" % (kind,))
        if kind in blocks:
            raise ValueError("duplicate block %r" % (kind,))
        blocks[kind] = data[start : start + size]
    return blocks


def _read_rerl(blob: bytes):
    table_offset, count, pad = struct.unpack_from("<III", blob, 0)
    if table_offset != 12 or pad != 0:
        raise ValueError("unexpected RERL header")
    entries = []
    for index in range(count):
        position = table_offset + 16 * index
        resource_id, name_offset = struct.unpack_from("<Qq", blob, position)
        name_at = position + 8 + name_offset
        end = blob.index(b"\x00", name_at)
        entries.append((resource_id, blob[name_at:end].decode("utf-8")))
    return entries


def _read_kv3(blob: bytes):
    (magic,) = struct.unpack_from("<I", blob, 0)
    if magic != KV3_MAGIC_V3:
        raise ValueError("not a KV3 v3 block: 0x%08x" % magic)
    if blob[4:20] != KV3_FORMAT_GUID:
        raise ValueError("unexpected format GUID")
    (method,) = struct.unpack_from("<I", blob, 20)
    dictionary_id, frame_size = struct.unpack_from("<HH", blob, 24)
    count1, count4, count8, count_types = struct.unpack_from("<4i", blob, 28)
    object_count, array_count = struct.unpack_from("<HH", blob, 44)
    size_uncompressed, size_compressed = struct.unpack_from("<ii", blob, 48)
    block_count, blob_bytes = struct.unpack_from("<ii", blob, 56)
    if method != 0 or dictionary_id != 0 or frame_size != 0:
        raise ValueError("expected uncompressed KV3 with no dictionary/frame")
    if block_count != 0 or blob_bytes != 0:
        raise ValueError("unexpected binary blocks")
    if size_compressed != size_uncompressed:
        raise ValueError("uncompressed KV3 must have equal sizes")
    raw = blob[64 : 64 + size_uncompressed]
    if len(raw) != size_uncompressed:
        raise ValueError("truncated KV3 payload")

    offset = count1
    offset = (offset + 3) & ~3
    ints = list(struct.unpack_from("<%di" % count4, raw, offset))
    offset += count4 * 4
    offset = (offset + 7) & ~7
    doubles = list(struct.unpack_from("<%dd" % count8, raw, offset)) if count8 else []
    offset += count8 * 8
    if count8 == 0:
        offset = (offset + 7) & ~7

    string_count = ints[0]
    string_start = offset
    strings = []
    for _ in range(string_count):
        end = raw.index(b"\x00", offset)
        strings.append(raw[offset:end].decode("utf-8"))
        offset = end + 1
    types_length = count_types - (offset - string_start)
    types = raw[offset : offset + types_length]
    offset += types_length
    (trailer,) = struct.unpack_from("<I", raw, offset)
    if trailer != KV3_TRAILER:
        raise ValueError("bad trailer 0x%08x" % trailer)
    offset += 4
    if offset != len(raw):
        raise ValueError("KV3 payload has %d trailing bytes" % (len(raw) - offset))

    state = {"i": 1, "d": 0, "t": 0, "flags": []}

    def take_int():
        value = ints[state["i"]]
        state["i"] += 1
        return value

    def take_double():
        value = doubles[state["d"]]
        state["d"] += 1
        return value

    def read_type():
        code = types[state["t"]]
        state["t"] += 1
        flag = 0
        if code & 0x80:
            code &= 0x3F
            flag = types[state["t"]]
            state["t"] += 1
        return code, flag

    def read_value(code, flag):
        if code == 1:
            return None
        if code == 2:
            raise ValueError("byte lane unused by the generator")
        if code in (3, 4):
            raise ValueError("64-bit lane unused by the generator")
        if code == 5:
            return take_double()
        if code == 6:
            index = take_int()
            if flag:
                return ("resource", strings[index])
            return strings[index]
        if code == 8:
            return [read_node() for _ in range(take_int())]
        if code == 10:
            length = take_int()
            sub_code, sub_flag = read_type()
            return [read_value(sub_code, sub_flag) for _ in range(length)]
        if code == 24:
            raise ValueError("byte-length arrays are not emitted by the generator")
        if code == 9:
            members = {}
            for _ in range(take_int()):
                child_code, child_flag = read_type()
                name = strings[take_int()]
                members[name] = read_value(child_code, child_flag)
            return members
        if code == 11:
            return take_int()
        if code in (13, 14):
            return code == 13
        if code in (15, 16):
            return code - 15
        if code in (17, 18):
            return float(code - 17)
        if code == 19:
            return struct.unpack("<f", struct.pack("<i", take_int()))[0]
        raise ValueError("unsupported node type %d" % code)

    def read_node():
        code, flag = read_type()
        return read_value(code, flag)

    document = read_node()
    if state["i"] != len(ints) or state["d"] != len(doubles) or state["t"] != len(types):
        raise ValueError("KV3 pools were not fully consumed")
    return document, (object_count, array_count)


def _close(actual, expected, tolerance=1e-5):
    """FLOAT nodes round-trip through IEEE-754 single precision."""
    if actual is None:
        return False
    return abs(float(actual) - float(expected)) <= tolerance * max(1.0, abs(float(expected)))


def _close_list(actual, expected, tolerance=1e-5):
    if not isinstance(actual, list) or len(actual) != len(expected):
        return False
    return all(_close(a, b, tolerance) for a, b in zip(actual, expected))


def verify(data: bytes) -> dict:
    """Decode ``data`` from scratch and check the authored invariants."""
    blocks = _read_container(data)
    if set(blocks) != {b"RERL", b"DATA"}:
        raise ValueError("unexpected blocks %r" % sorted(blocks))
    entries = _read_rerl(blocks[b"RERL"])
    expected_id = murmur_hash64b(BOLT_TEXTURE.encode("utf-8"))
    if entries != [(expected_id, BOLT_TEXTURE)]:
        raise ValueError("unexpected external reference %r" % (entries,))
    document, counts = _read_kv3(blocks[b"DATA"])

    if document.get("_class") != "CParticleSystemDefinition":
        raise ValueError("root class is not CParticleSystemDefinition")
    if document.get("m_nBehaviorVersion") != 4:
        raise ValueError("behavior version must be 4")
    if document.get("m_nMaxParticles") != 32:
        raise ValueError("max particles must be 32")
    if document.get("m_ConstantColor") != [238, 246, 255, 255]:
        raise ValueError("constant colour must be cool white")

    renderers = document.get("m_Renderers") or []
    if len(renderers) != 1:
        raise ValueError("expected exactly one renderer")
    renderer = renderers[0]
    if renderer.get("_class") != "C_OP_RenderRopes":
        raise ValueError("renderer is not a rope renderer")
    if renderer.get("m_bDisableOperator") is not False:
        raise ValueError("rope renderer operator must be enabled")
    if renderer.get("m_nOutputBlendMode") != "PARTICLE_OUTPUT_BLEND_MODE_ADD":
        raise ValueError("rope renderer must be additive")
    if not _close(renderer.get("m_flOverbrightFactor"), 10.0):
        raise ValueError("rope overbright must be 10")
    if not _close(renderer.get("m_flRadiusScale"), 0.65):
        raise ValueError("rope radius scale must be 0.65")
    if not _close(renderer.get("m_flTextureVWorldSize"), 128.0):
        raise ValueError("rope texture world size must be 128")
    textures = renderer.get("m_vecTexturesInput") or []
    if len(textures) != 1 or textures[0].get("m_hTexture") != ("resource", BOLT_TEXTURE):
        raise ValueError("texture is not the flagged bendibeam resource reference")

    initializers = document.get("m_Initializers") or []
    classes = [item.get("_class") for item in initializers]
    if classes[:1] != ["C_INIT_CreateSequentialPath"]:
        raise ValueError("first initializer must be the sequential path")
    path = initializers[0]
    if not _close(path.get("m_flNumToAssign"), 32.0):
        raise ValueError("sequential path must assign 32 points")
    params = path.get("m_PathParams") or {}
    if params.get("m_nStartControlPointNumber") != 0 or params.get("m_nEndControlPointNumber") != 1:
        raise ValueError("sequential path must run from CP0 to CP1")
    offset_initializer = next((item for item in initializers if item.get("_class") == "C_INIT_PositionOffset"), None)
    if offset_initializer is None:
        raise ValueError("missing position offset initializer")
    if not _close_list(offset_initializer.get("m_OffsetMin"), [-24.0, -24.0, -3.0]):
        raise ValueError("position offset min must be +/-24 XY and +/-3 Z")
    if not _close_list(offset_initializer.get("m_OffsetMax"), [24.0, 24.0, 3.0]):
        raise ValueError("position offset max must be +/-24 XY and +/-3 Z")
    radius = next((item for item in initializers if item.get("_class") == "C_INIT_RandomRadius"), None)
    if radius is None or not _close(radius.get("m_flRadiusMin"), 4.0) or not _close(radius.get("m_flRadiusMax"), 6.0):
        raise ValueError("radius initializer must be 4..6")
    lifetime = next((item for item in initializers if item.get("_class") == "C_INIT_RandomLifeTime"), None)
    if lifetime is None or not _close(lifetime.get("m_flLifetimeMin"), 0.20) \
            or not _close(lifetime.get("m_flLifetimeMax"), 0.24):
        raise ValueError("lifetime initializer must be ~0.22 s")
    alpha = next((item for item in initializers if item.get("_class") == "C_INIT_RandomAlpha"), None)
    if alpha is None or alpha.get("m_nAlphaMin") != 255 or alpha.get("m_nAlphaMax") != 255:
        raise ValueError("alpha initializer must pin alpha to 1")

    operators = [item.get("_class") for item in document.get("m_Operators") or []]
    if operators != ["C_OP_BasicMovement", "C_OP_Decay", "C_OP_FadeOut"]:
        raise ValueError("unexpected operator set %r" % (operators,))
    emitters = document.get("m_Emitters") or []
    if len(emitters) != 1 or emitters[0].get("_class") != "C_OP_InstantaneousEmitter":
        raise ValueError("expected one instantaneous emitter")
    particles = emitters[0].get("m_nParticlesToEmit") or {}
    if particles.get("m_nType") != "PF_TYPE_LITERAL" or not _close(particles.get("m_flLiteralValue"), 32.0):
        raise ValueError("emitter must emit 32 particles at once")

    return {
        "class": document["_class"],
        "resourceId": "%016x" % expected_id,
        "objects": counts[0],
        "arrays": counts[1],
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, help="path of the .vpcf_c resource to write")
    parser.add_argument("--check", action="store_true", help="verify the written file after writing")
    parser.add_argument("--json", action="store_true", help="print the verification summary as JSON")
    args = parser.parse_args(argv)

    if murmur_hash64b(BOLT_TEXTURE.encode("utf-8")) != 0x6E4A70008D0EE530:
        print("error: resource-hash self-check failed", file=sys.stderr)
        return 2

    data = build_resource()
    summary = verify(data)

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)

    if args.check:
        if output.read_bytes() != data:
            print("error: written file differs from the generated bytes", file=sys.stderr)
            return 3
        summary = verify(output.read_bytes())

    if args.json:
        print(json.dumps(summary, sort_keys=True))
    else:
        print(
            "generated %s (%d bytes, sha256=%s, resource=%s)"
            % (output, summary["size"], summary["sha256"], summary["resourceId"])
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
