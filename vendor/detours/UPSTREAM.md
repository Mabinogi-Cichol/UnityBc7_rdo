# Microsoft Detours

- Repository: https://github.com/microsoft/Detours
- Version: 4.0.1
- Commit: `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`
- License: MIT, retained verbatim in `LICENSE.md` and source headers.
- Imported: `src/`, `README.md`, `LICENSE.md`. Upstream files are unmodified.
- Local build: MSVC x64, `/MT /O2 /EHsc /std:c++17`; static `detours.lib`.
- This dependency is used by the RGBM binding tools, production Hook contract
  validation and the independent installation coordinator, as well as tests.
- P2 additionally compiles `src/detours_transaction_engine.cpp`, which includes
  the unchanged upstream `detours.cpp` with scoped nothrow allocation and checked
  Windows API adapters. Its dedicated coordinator DLL has a VirtualAlloc-based
  C++ allocator. See `docs/features/rgbm-hook-portability-detours.md`; no upstream
  source edits or debug tracing are enabled.
