# File Formats

## `book.bin`

### Version 3

ImHex Pattern:

```c++
import std.mem;
import std.string;
import std.core;

// === Configuration ===
#define EXPECTED_VERSION 3
#define MAX_STRING_LENGTH 65535

// === String Structure ===

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

// === Metadata Structure ===

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
} [[comment("Book metadata information")]];

// === Spine Entry Structure ===

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative size in bytes"), color("FF6B6B")]];
    s16 tocIndex [[comment("Index into TOC (-1 if none)"), color("4ECDC4")]];
} [[comment("Spine entry defining reading order")]];

// === TOC Entry Structure ===

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level (0-255)"), color("95E1D3")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)"), color("F38181")]];
} [[comment("Table of contents entry")]];

// === Book Bin Structure ===

struct BookBin {
    // Header
    u8 version [[comment("Format version"), color("FFD93D")]];
    
    // Version validation
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }
    
    u32 lutOffset [[comment("Offset to lookup tables"), color("6BCB77")]];
    u16 spineCount [[comment("Number of spine entries"), color("4D96FF")]];
    u16 tocCount [[comment("Number of TOC entries"), color("FF6B9D")]];
    
    // Metadata section
    Metadata metadata [[comment("Book metadata")]];
    
    // Validate LUT offset alignment
    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }
    
    // Lookup Tables
    u32 spineLut[spineCount] [[comment("Spine entry offsets"), color("4D96FF")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets"), color("FF6B9D")]];
    
    // Data Entries
    SpineEntry spines[spineCount] [[comment("Spine entries (reading order)")]];
    TocEntry toc[tocCount] [[comment("Table of contents entries")]];
};

// === File Parsing ===

BookBin book @ 0x00;

// Validate we've consumed the entire file
u32 fileSize = std::mem::size();
u32 parsedSize = $;

if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## 북마크 저장 형식

북마크는 도서 캐시와 분리된 `/.crosspoint/bookmarks` 디렉터리에 도서별 JSON 파일로 저장됩니다. 따라서 EPUB
레이아웃 캐시를 삭제하거나 다시 생성해도 북마크는 유지됩니다. 파일 이름은 도서 내용 지문으로 만들기 때문에 파일을
다른 폴더로 옮기거나 이름을 바꿔도 같은 북마크 파일을 사용합니다.

```json
{
  "version": 2,
  "bookId": "82a112dd0f645cf8",
  "bookPath": "/books/example.epub",
  "bookmarks": [
    {
      "spineIndex": 3,
      "page": 12,
      "pageCount": 40,
      "bookProgress": 27,
      "chapter": "3장"
    }
  ]
}
```

- `bookId`: 파일 크기와 앞·중간·끝 내용을 조합한 64비트 도서 지문입니다.
- `bookPath`: 마지막으로 북마크를 저장한 경로이며 도서 식별에는 사용하지 않습니다.
- `spineIndex`: EPUB의 스파인 위치입니다. XTC와 텍스트처럼 페이지 기반 형식은 `-1`을 사용합니다.
- `page`: 0부터 시작하는 현재 페이지입니다.
- `pageCount`: 북마크를 저장할 당시 챕터 또는 문서의 전체 페이지 수입니다.
- `bookProgress`: 화면에 표시할 전체 도서 진행률(0~100)입니다.
- `chapter`: 북마크 목록에 표시할 챕터 제목입니다.

EPUB은 글꼴이나 여백 설정에 따라 페이지 수가 달라질 수 있으므로, 이동할 때 저장된 `page/pageCount` 비율을 현재
레이아웃에 적용합니다. 한 도서에는 최대 50개의 북마크를 저장합니다.

기존 버전 1 북마크는 레지스트리에 기록된 과거 경로를 사용해 버전 2 파일로 자동 복사합니다. 롤백할 수 있도록 기존
파일은 삭제하지 않습니다.

## 도서 데이터 레지스트리

`/.crosspoint/book-data.json`은 도서 내용 지문과 캐시 키, 현재 경로 및 과거 경로를 연결합니다. 기존 경로 기반
캐시가 있으면 그 키를 그대로 등록하므로 큰 EPUB 캐시를 이동하거나 다시 만들지 않습니다. 새 도서는 내용 지문 기반
키를 사용합니다.

도서 지문은 파일 크기와 최대 4KB인 앞·중간·끝 표본을 FNV-1a 64비트로 계산합니다. 작은 파일은 전체 내용을
사용합니다. 펌웨어 업데이트 후 현재 최근 도서에 들어 있는 파일은 자동 등록되며, 이후 파일 이름이나 폴더가 바뀌면
같은 진행률 캐시와 북마크를 다시 찾을 수 있습니다.

## `section.bin`

### Version 8

ImHex Pattern:

```c++
import std.mem;
import std.string;
import std.core;

// === Configuration ===
#define EXPECTED_VERSION 8
#define MAX_STRING_LENGTH 65535

// === String Structure ===

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

// === Page Structure ===

enum StorageType : u8 {
    PageLine = 1
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3
};

enum BlockStyle : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
};

struct PageLine {
  s16 xPos;
  s16 yPos;
  u16 wordCount;
  String words[wordCount];
  u16 wordXPos[wordCount];
  WordStyle wordStyle[wordCount];
  BlockStyle blockStyle;
};

struct PageElement {
    u8 pageElementType;
    if (pageElementType == 1) {
        PageLine pageLine [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];
};

// === Section Bin Structure ===

struct SectionBin {
    // Header
    u8 version [[comment("Format version"), color("FFD93D")]];
    
    // Version validation
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }
    
    // Cache busting parameters
    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    u16 viewportWidth;
    u16 vieportHeight;
    u16 pageCount;
    u32 lutOffset;
    
    Page page[pageCount];
    
    // Validate LUT offset alignment
    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }
    
    // Lookup Tables
    u32 lut[pageCount];
};

// === File Parsing ===

SectionBin book @ 0x00;

// Validate we've consumed the entire file
u32 fileSize = std::mem::size();
u32 parsedSize = $;

if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```
