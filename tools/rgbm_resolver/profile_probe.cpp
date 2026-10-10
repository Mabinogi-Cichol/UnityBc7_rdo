// Developer-only fingerprint extraction. Does NOT certify or publish a cache.
#include "rgbm_hook_contract.h"
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
    if (argc != 4) return 2;
    rgbm::PeImage image; std::string error;
    if (!image.load(argv[1], error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
    wchar_t* end; auto rva = wcstoul(argv[2], &end, 16); if (*end) return 2;
    auto length = wcstoul(argv[3], &end, 10); if (*end) return 2;
    auto hash = rgbm::canonical_body_hash(image, rva, length);
    if (hash.empty()) return 1;
    puts(hash.c_str()); return 0;
}
