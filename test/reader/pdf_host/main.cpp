#include <PdfText.h>
#include <cstdio>

int main(int argc, char** argv) {
  if (argc != 4) return 100;
  const auto error = PdfText::extract(argv[1], argv[2], argv[3]);
  std::printf("%d\n", static_cast<int>(error));
  return static_cast<int>(error);
}
