"""PlatformIO 빌드 후 글꼴 assets 이미지와 전체 USB 플래시 이미지를 생성한다."""

from pathlib import Path
import sys

Import("env")

PROJECT_DIR = Path(env["PROJECT_DIR"])
BUILD_DIR = Path(env.subst("$BUILD_DIR"))
ASSET_OFFSET = 0x810000
FLASH_SIZE = 0x1000000

sys.path.insert(0, str(PROJECT_DIR / "scripts"))
from pack_font_assets import build_asset_image  # noqa: E402


def place(image: bytearray, offset: int, data: bytes, label: str) -> None:
    end = offset + len(data)
    if end > len(image):
        raise RuntimeError(f"{label} 이미지가 16MB 플래시 범위를 초과합니다: 0x{end:X}")
    image[offset:end] = data


def build_images(source, target, env) -> None:
    assets_path = BUILD_DIR / "assets.bin"
    total, ui_size, reader_size = build_asset_image(PROJECT_DIR, assets_path)

    full_image = bytearray(b"\xFF" * FLASH_SIZE)
    place(full_image, 0x0000, (BUILD_DIR / "bootloader.bin").read_bytes(), "부트로더")
    place(full_image, 0x8000, (BUILD_DIR / "partitions.bin").read_bytes(), "파티션 테이블")
    place(full_image, 0x10000, Path(str(target[0])).read_bytes(), "펌웨어")
    place(full_image, ASSET_OFFSET, assets_path.read_bytes(), "글꼴 assets")

    full_path = BUILD_DIR / "firmware-full.bin"
    full_path.write_bytes(full_image)
    print(
        f"전체 USB 플래시 이미지 생성: {full_path} "
        f"(assets {total:,}바이트: UI {ui_size:,}, 리더 {reader_size:,})"
    )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", build_images)
