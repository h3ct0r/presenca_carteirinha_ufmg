# PlatformIO pre-script: pack the esp-dl .espdl models into the flash-partition
# image the firmware loads them from (`human_face_det`, see partitions_model.csv).
#
# Why this exists: esp-dl's own `pack_models` tool is not vendored here, but the
# vendored *loader* (lib/esp-dl/fbs_loader/src/fbs_loader.cpp) defines the format
# exactly, so we generate it ourselves. That file is the authority — if a future
# esp-dl bump changes the container, it changes there first.
#
# Container (PDL2), per fbs_loader.cpp's pack_* helpers:
#
#     0x00  "PDL2"                        magic
#     0x04  model_num                     u32
#     0x08  entries[model_num]            3 x u32: {data_offset, name_offset, name_len}
#           ...names, then the model blobs
#
# Each blob is a source .espdl copied verbatim: those files are EDL2
# ("EDL2", mode, size, pad = a 16-byte header), which is precisely the per-model
# record create_fbs_model() expects to find at `base + data_offset`.
Import("env")  # type: ignore  # PlatformIO injects Import

import os
import struct
import sys

# The names esp-dl asks for by string (human_face_detect.cpp load_model()).
# They must round-trip through the pack exactly or the lookup fails at runtime.
MODELS = [
    "human_face_detect_msr_s8_v1.espdl",
    "human_face_detect_mnp_s8_v1.espdl",
]

SRC_DIR = "models"
OUT_NAME = "human_face_det.bin"
PARTITION_LABEL = "human_face_det"
PARTITION_CSV = "partitions_model.csv"

MAGIC = b"PDL2"
ENTRY_SIZE = 12  # 3 x u32
# create_fbs_model() hands the flatbuffer at `base + data_offset + 16` straight
# to the parser and only takes the zero-copy path when that address is 16-byte
# aligned; otherwise it warns and copies every parameter to PSRAM instead.
ALIGN = 16


def _fail(msg):
    print("pack_models: %s" % msg, file=sys.stderr)
    env.Exit(1)


def _pad_to(buf, alignment):
    if len(buf) % alignment:
        buf.extend(b"\0" * (alignment - len(buf) % alignment))


def _partition(csv_path):
    """Offset and size of the model partition, read from the CSV that is also
    flashed to the device — so the image, the flash address and the table can
    never disagree."""
    if not os.path.isfile(csv_path):
        _fail("missing %s" % csv_path)
    for line in open(csv_path):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) >= 5 and cols[0] == PARTITION_LABEL:
            return int(cols[3], 0), int(cols[4], 0)
    _fail("no '%s' row in %s" % (PARTITION_LABEL, csv_path))


def _pack(src_dir):
    blobs = []
    for name in MODELS:
        path = os.path.join(src_dir, name)
        if not os.path.isfile(path):
            _fail("missing model %s" % path)
        with open(path, "rb") as f:
            data = f.read()
        # Sanity: the per-model header the loader will read back out.
        if data[:4] != b"EDL2":
            _fail("%s is not an EDL2 model (magic %r)" % (name, data[:4]))
        blobs.append((name.encode("utf-8"), data))

    header_size = 8 + ENTRY_SIZE * len(blobs)
    out = bytearray(header_size)
    out[0:4] = MAGIC
    struct.pack_into("<I", out, 4, len(blobs))

    # Names first (they are small and unaligned), then each blob on an ALIGN
    # boundary so the flatbuffer inside it lands aligned too.
    name_offsets = []
    for name, _ in blobs:
        name_offsets.append(len(out))
        out.extend(name)

    for i, (name, data) in enumerate(blobs):
        _pad_to(out, ALIGN)
        data_offset = len(out)
        out.extend(data)
        struct.pack_into(
            "<III", out, 8 + i * ENTRY_SIZE, data_offset, name_offsets[i], len(name)
        )

    return bytes(out)


def _verify(image, partition_size):
    """Re-read the image the way fbs_loader.cpp does. This is the only check
    available before the thing is on a device, so it is deliberately literal."""
    if image[:4] != MAGIC:
        _fail("bad magic in generated image")
    (count,) = struct.unpack_from("<I", image, 4)
    if count != len(MODELS):
        _fail("expected %d models, header says %d" % (len(MODELS), count))

    for i, expected in enumerate(MODELS):
        data_off, name_off, name_len = struct.unpack_from(
            "<III", image, 8 + i * ENTRY_SIZE
        )
        name = image[name_off : name_off + name_len].decode("utf-8", "replace")
        if name != expected:
            _fail("entry %d resolves to %r, expected %r" % (i, name, expected))
        if data_off % ALIGN:
            _fail("entry %d data offset 0x%x is not %d-byte aligned" % (i, data_off, ALIGN))
        if image[data_off : data_off + 4] != b"EDL2":
            _fail("entry %d does not point at an EDL2 record" % i)
        # header[2] is the model size the loader trusts; it must fit the image.
        (size,) = struct.unpack_from("<I", image, data_off + 8)
        if data_off + 16 + size > len(image):
            _fail("entry %d claims %d bytes, past the end of the image" % (i, size))

    if len(image) > partition_size:
        _fail(
            "image is %d B, larger than the %d B %s partition"
            % (len(image), partition_size, PARTITION_LABEL)
        )


project_dir = env.subst("$PROJECT_DIR")
part_offset, part_size = _partition(os.path.join(project_dir, PARTITION_CSV))

image = _pack(os.path.join(project_dir, SRC_DIR))
_verify(image, part_size)

out_path = os.path.join(env.subst("$BUILD_DIR"), OUT_NAME)
os.makedirs(os.path.dirname(out_path), exist_ok=True)
with open(out_path, "wb") as f:
    f.write(image)

# Make `pio run -t upload` write it too. Without this the partition stays erased
# and esp-dl reads 0xFF..., reports "Model's flatbuffers is empty or broken", and
# then dereferences the null model — a panic, not a graceful failure.
#
# Guarded: PlatformIO runs this script in more than one scope, and appending
# unconditionally makes esptool write the same image twice.
flash_entry = (hex(part_offset), os.path.join("$BUILD_DIR", OUT_NAME))
if flash_entry not in env.get("FLASH_EXTRA_IMAGES", []):
    env.Append(FLASH_EXTRA_IMAGES=[flash_entry])

print(
    "pack_models: %s <- %d models, %d B (%.0f%% of the %d B partition) -> flash at %s"
    % (
        OUT_NAME,
        len(MODELS),
        len(image),
        100.0 * len(image) / part_size,
        part_size,
        hex(part_offset),
    )
)
