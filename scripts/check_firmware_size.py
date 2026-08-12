"""ESP32-C3에서 부팅할 수 없는 8 MiB 초과 앱 이미지를 차단한다."""

from pathlib import Path

Import("env")


MAX_BOOTABLE_IMAGE_SIZE = 8 * 1024 * 1024


def check_firmware_size(source, target, env):
    firmware_path = Path(str(target[0]))
    firmware_size = firmware_path.stat().st_size
    remaining = MAX_BOOTABLE_IMAGE_SIZE - firmware_size

    if remaining < 0:
        raise RuntimeError(
            "ESP32-C3 앱 이미지가 8 MiB 부팅 한도를 "
            f"{-remaining:,}바이트 초과했습니다: {firmware_size:,}바이트"
        )

    print(
        "ESP32-C3 앱 이미지 크기 확인: "
        f"{firmware_size:,}바이트, 남은 여유 {remaining:,}바이트"
    )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_firmware_size)
