#pragma once

#include <string>

#include "PdfTextParser.h"

namespace PdfText {
// 결과는 임시 UTF-8 텍스트이며 실패하면 삭제한다. 입력 PDF는 읽기 전용으로 연다.
Error extract(const std::string& input, const std::string& output, const std::string& cacheDirectory);
}  // namespace PdfText
