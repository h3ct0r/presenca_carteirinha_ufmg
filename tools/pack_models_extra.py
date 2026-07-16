# PlatformIO extra script: pack the .espdl models into a single image and
# register it for flashing into the human_face_det partition (see
# partitions_model.csv - keep MODEL_PARTITION_OFFSET in sync with it).
#
# The models (~1MB) almost never change, so by default uploads skip them to
# keep flashing fast. Flash them explicitly (first board bring-up, or after
# changing the model files) with either:
#     pio run -e esp32p4_models -t upload     (env with custom_flash_models)
#     PIO_FLASH_MODELS=1 pio run -t upload    (env var override)
Import("env")
import os
import subprocess

MODEL_PARTITION_OFFSET = "0x610000"

proj = env["PROJECT_DIR"]
build_dir = env.subst("$BUILD_DIR")
models = [
    os.path.join(proj, "lib", "human_face_detect", "models", "p4", "human_face_detect_msr_s8_v1.espdl"),
    os.path.join(proj, "lib", "human_face_detect", "models", "p4", "human_face_detect_mnp_s8_v1.espdl"),
]
packed = os.path.join(build_dir, "human_face_detect.espdl")
pack_script = os.path.join(proj, "tools", "pack_espdl_models.py")


def _needs_pack():
    if not os.path.exists(packed):
        return True
    packed_mtime = os.path.getmtime(packed)
    return any(os.path.getmtime(m) > packed_mtime for m in models)


if _needs_pack():
    os.makedirs(build_dir, exist_ok=True)
    subprocess.check_call(
        [env.subst("$PYTHONEXE"), pack_script, "--model_path", *models, "--out_file", packed]
    )
    print(f"Packed espdl models -> {packed}")

flash_models = (
    os.environ.get("PIO_FLASH_MODELS") == "1"
    or str(env.GetProjectOption("custom_flash_models", "no")).lower() in ("1", "yes", "true")
)

if flash_models:
    env.Append(FLASH_EXTRA_IMAGES=[(MODEL_PARTITION_OFFSET, packed)])
    print(f"Model partition WILL be flashed at {MODEL_PARTITION_OFFSET}")
else:
    print("Skipping model partition (use -e esp32p4_models to flash it)")
