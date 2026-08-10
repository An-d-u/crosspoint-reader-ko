from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/network/CrossPointWebServer.cpp"


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    required = (
        'id="dropZone" class="drop-zone"',
        "dropZone.addEventListener('dragenter'",
        "dropZone.addEventListener('dragover'",
        "dropZone.addEventListener('dragleave'",
        "dropZone.addEventListener('drop'",
        "dropZone.addEventListener('keydown'",
        "const transfer = new DataTransfer()",
        "input.files = transfer.files",
        "if (input.disabled || !event.dataTransfer",
        "button.disabled = true",
        "input.disabled = true",
        "button.disabled = false",
        "input.disabled = false",
    )
    missing = [token for token in required if token not in source]
    if missing:
        print(f"드래그 앤 드롭 업로드 구성 요소가 누락되었습니다: {missing}", file=sys.stderr)
        return 1

    drop_handler = source.find("dropZone.addEventListener('drop'")
    if source.find("input.files = transfer.files", drop_handler) < drop_handler:
        print("드롭한 파일이 실제 업로드 입력 요소에 연결되지 않았습니다", file=sys.stderr)
        return 1

    print("확인 완료: 웹 업로드 드래그 앤 드롭 흐름")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
