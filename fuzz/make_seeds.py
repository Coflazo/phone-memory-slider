"""Writes the seed corpora in fuzz/corpus/<target>/ from the golden fixtures and small hand-built file heads.

Run from the repository root: python fuzz/make_seeds.py. Standard library only; the output is deterministic.
"""
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "core" / "tests" / "fixtures"
CORPUS = ROOT / "fuzz" / "corpus"


def length_prefixed(data):
    """Splits u16 BE length + bytes records (golden_messages.bin, golden_ik_transcript.bin)."""
    out, at = [], 0
    while at < len(data):
        n = int.from_bytes(data[at:at + 2], "big")
        out.append(data[at + 2:at + 2 + n])
        at += 2 + n
    return out


def frame(payload):
    return len(payload).to_bytes(2, "big") + payload


def apple_tiff():
    """IFD0 Make "Apple" and an ExifIFD MakerNote with BurstUUID and ContentIdentifier, big-endian (media_meta_tests)."""
    b = bytearray(b"MM" + struct.pack(">HI", 42, 8))
    b += struct.pack(">H", 2)
    b += struct.pack(">HHI", 0x010F, 2, 6)
    make_off = len(b)
    b += b"\0\0\0\0"
    b += struct.pack(">HHI", 0x8769, 4, 1)
    exif_ptr = len(b)
    b += b"\0\0\0\0" + b"\0\0\0\0"
    b[make_off:make_off + 4] = struct.pack(">I", len(b))
    b += b"Apple\0"
    b[exif_ptr:exif_ptr + 4] = struct.pack(">I", len(b))
    burst, content_id = b"burst-uuid-1", b"content-id-1"
    note = bytearray(b"Apple iOS\0\0\x01MM" + struct.pack(">H", 2))
    burst_off = len(note) + 8
    note += struct.pack(">HHI", 0x000B, 2, len(burst) + 1) + b"\0\0\0\0"
    cid_off = len(note) + 8
    note += struct.pack(">HHI", 0x0011, 2, len(content_id) + 1) + b"\0\0\0\0" + b"\0\0\0\0"
    note[burst_off:burst_off + 4] = struct.pack(">I", len(note))
    note += burst + b"\0"
    note[cid_off:cid_off + 4] = struct.pack(">I", len(note))
    note += content_id + b"\0"
    b += struct.pack(">H", 1)
    b += struct.pack(">HHI", 0x927C, 7, len(note))
    mn_ptr = len(b)
    b += b"\0\0\0\0" + b"\0\0\0\0"
    b[mn_ptr:mn_ptr + 4] = struct.pack(">I", len(b))
    b += note
    return bytes(b)


MOTION_XMP = (
    b'<x:xmpmeta><rdf:RDF><rdf:Description Camera:MotionPhoto="1" Camera:MotionPhotoVersion="1" '
    b'Camera:MotionPhotoPresentationTimestampUs="1234"><Container:Directory><rdf:Seq>'
    b'<rdf:li><Container:Item Item:Mime="image/jpeg" Item:Semantic="Primary" Item:Length="0"/></rdf:li>'
    b'<rdf:li><Container:Item Item:Mime="video/mp4" Item:Semantic="MotionPhoto" Item:Length="345678"/></rdf:li>'
    b"</rdf:Seq></Container:Directory></rdf:Description></rdf:RDF></x:xmpmeta>"
)


def jpeg(exif, xmp):
    """SOI, APP0, APP1 Exif, APP1 XMP, SOS (media_meta_tests' jpeg_with)."""
    out = bytearray(b"\xFF\xD8\xFF\xE0\x00\x04\x00\x00")
    for signature, body in ((b"Exif\0\0", exif), (b"http://ns.adobe.com/xap/1.0/\0", xmp)):
        out += b"\xFF\xE1" + (2 + len(signature) + len(body)).to_bytes(2, "big") + signature + body
    return bytes(out + b"\xFF\xDA\x00\x02")


def ftyp(major, compat):
    body = major + b"\0\0\0\0" + b"".join(compat)
    return (8 + len(body)).to_bytes(4, "big") + b"ftyp" + body


def main():
    messages = length_prefixed((FIXTURES / "golden_messages.bin").read_bytes())
    transcript = length_prefixed((FIXTURES / "golden_ik_transcript.bin").read_bytes())
    msg1 = transcript[11]
    transport = [transcript[i] for i in (15, 17, 19, 21)]
    seeds = {
        "messages": {f"golden_{m[0]:02x}": m for m in messages},
        # Byte 0 bit 0 picks the pre-handshake cap, byte 1 the read size minus one.
        "frames": {
            "preauth_msg1": b"\x01\x0f" + frame(msg1),
            "transport_four": b"\x00\x3f" + b"".join(frame(ct) for ct in transport),
        },
        "qr": {
            "protocol_vector": b"PMS1:AEAQEAYEAUDAOCAJBIFQYDIOB4IBCEQTCQKRMFYYDENBWHA5DYPSAIJCEMSCKJRHFAUSUKZMFUXC6MBRGIZTINJWG44DSOR3HQ6T4P2AIFBEGRCFIZDUQSKK",
        },
        "handshake": {"golden_msg1": msg1},
        "media_meta": {
            "jpeg_apple_motion": jpeg(apple_tiff(), MOTION_XMP),
            "tiff_apple": apple_tiff(),
            "xmp_motion": MOTION_XMP,
            "xmp_microvideo": b'<x GCamera:MicroVideo="1" GCamera:MicroVideoOffset="99000"/>',
        },
        "sniff": {
            "jpeg": b"\xFF\xD8\xFF\xE0\x00\x10JFIF\0",
            "apng": b"\x89PNG\r\n\x1a\n" + b"\0\0\0\x0dIHDR" + bytes(17) + b"\0\0\0\x08acTL" + bytes(12),
            "gif": b"GIF89a" + bytes(7) + b"\x21\xF9\x04" + bytes(5) + b"\x2C" + bytes(9) + b"\x21\xF9\x04",
            "webp": b"RIFF\x1e\0\0\0WEBPVP8X" + bytes(14),
            "heic": ftyp(b"heic", [b"mif1", b"heic"]),
            "heif_sequence": ftyp(b"msf1", [b"msf1", b"hevc"]),
            "avif": ftyp(b"avif", [b"avif", b"mif1"]),
            "mov": ftyp(b"qt  ", [b"qt  "]),
            "mp4": ftyp(b"isom", [b"isom", b"avc1"]),
            "3gp": ftyp(b"3gp4", [b"isom", b"3gp4"]),
            "webm": bytes([0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x86, 0x81, 0x01, 0x42, 0x82, 0x84]) + b"webm",
            "mkv": bytes([0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x82, 0x88]) + b"matroska",
            "avi": b"RIFF\0\1\0\0AVI LIST",
            "tiff": b"II*\0\x08\0\0\0",
        },
    }
    for target, files in seeds.items():
        directory = CORPUS / target
        directory.mkdir(parents=True, exist_ok=True)
        for name, data in files.items():
            (directory / name).write_bytes(data)
        print(f"{target}: {len(files)} seeds")


if __name__ == "__main__":
    main()
