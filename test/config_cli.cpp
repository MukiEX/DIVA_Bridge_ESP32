// Host harness for ConfigBlob: "decode <file>" prints serial + payload,
// "encode <serial> <payload-file> <out>" writes a 4 KB blob.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../src/ConfigBlob.h"
static std::vector<uint8_t> slurp(const char *p) {
  std::vector<uint8_t> v; FILE *f = fopen(p, "rb"); int c;
  while ((c = fgetc(f)) != EOF) v.push_back(c);
  fclose(f); return v;
}
int main(int argc, char **argv) {
  if (!strcmp(argv[1], "decode")) {
    auto b = slurp(argv[2]); const char *p; size_t n; uint32_t s;
    if (!ConfigBlob::decode(b.data(), b.size(), p, n, s)) { puts("INVALID"); return 0; }
    printf("%u\n", s); fwrite(p, 1, n, stdout); return 0;
  }
  auto pl = slurp(argv[3]); std::vector<uint8_t> out(ConfigBlob::PARTITION_SIZE);
  if (!ConfigBlob::encode((const char *)pl.data(), pl.size(), strtoul(argv[2], 0, 10), out.data())) return 1;
  FILE *f = fopen(argv[4], "wb"); fwrite(out.data(), 1, out.size(), f); fclose(f);
}
