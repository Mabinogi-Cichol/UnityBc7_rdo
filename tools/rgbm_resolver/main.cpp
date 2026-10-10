// Phase P0/P1 evidence collector. Reports are deliberately NOT installable caches.
// No Unity private address or compiler-generated lambda name is hardcoded here.
#include "rgbm_hook_bindings.h"
#include "rgbm_hook_contract.h"
#include <dia2.h>
#include <detours.h>
#include <dbghelp.h>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <memory>
#include <algorithm>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "diaguids.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "advapi32.lib")

namespace {
template<class T> struct ComDelete { void operator()(T* p) const { if (p) p->Release(); } };
template<class T> using Com = std::unique_ptr<T, ComDelete<T>>;
class LocalDiaModule {
    HMODULE module_ = nullptr;
public:
    HRESULT create(const std::wstring& path, IDiaDataSource** source) {
        *source = nullptr;
        module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) return HRESULT_FROM_WIN32(GetLastError());
        using GetClass = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
        auto getclass = reinterpret_cast<GetClass>(GetProcAddress(module_, "DllGetClassObject"));
        if (!getclass) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        IClassFactory* raw = nullptr;
        HRESULT hr = getclass(CLSID_DiaSource, IID_IClassFactory, reinterpret_cast<void**>(&raw));
        Com<IClassFactory> factory(raw);
        if (FAILED(hr) || !factory) return FAILED(hr) ? hr : E_UNEXPECTED;
        return factory->CreateInstance(nullptr, IID_IDiaDataSource, reinterpret_cast<void**>(source));
    }
    std::wstring path() const {
        std::wstring text(32768, L'\0');
        DWORD count = GetModuleFileNameW(module_, text.data(), DWORD(text.size()));
        if (!module_ || !count || count >= text.size()) return {};
        text.resize(count); return text;
    }
    ~LocalDiaModule() { if (module_) FreeLibrary(module_); }
};
std::string utf8(const std::wstring& text) {
    int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(count, '\0');
    if (count) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}
std::wstring symbol_name(IDiaSymbol* symbol) {
    BSTR text = nullptr;
    symbol->get_name(&text);
    std::wstring result = text ? text : L""; SysFreeString(text); return result;
}
struct Candidate {
    DWORD rva = 0, tag = 0; ULONGLONG length = 0;
    std::string name, signature;
    bool type_available = false;
};
std::string type_description(IDiaSymbol* type, unsigned depth = 0) {
    if (!type || depth > 8) return "unknown";
    DWORD tag = 0; type->get_symTag(&tag);
    auto name = symbol_name(type);
    if (!name.empty()) return utf8(name);
    IDiaSymbol* child = nullptr;
    if (tag == SymTagPointerType && SUCCEEDED(type->get_type(&child)) && child) {
        Com<IDiaSymbol> own(child); BOOL reference = FALSE; type->get_reference(&reference);
        return type_description(child, depth + 1) + (reference ? "&" : "*");
    }
    DWORD basic = 0; ULONGLONG length = 0;
    type->get_baseType(&basic); type->get_length(&length);
    return "tag=" + std::to_string(tag) + ":base=" + std::to_string(basic) + ":bytes=" + std::to_string(length);
}
std::string function_type(IDiaSymbol* symbol, bool& available) {
    IDiaSymbol* type = nullptr;
    if (symbol->get_type(&type) != S_OK || !type) return "";
    Com<IDiaSymbol> own(type); DWORD tag = 0; type->get_symTag(&tag);
    if (tag != SymTagFunctionType) return "";
    available = true;
    IDiaSymbol* ret = nullptr; type->get_type(&ret); Com<IDiaSymbol> ownret(ret);
    std::string signature = type_description(ret) + " (";
    IDiaEnumSymbols* args = nullptr;
    if (type->findChildren(SymTagFunctionArgType, nullptr, 0, &args) == S_OK && args) {
        Com<IDiaEnumSymbols> ownargs(args); IDiaSymbol* arg = nullptr; ULONG fetched = 0; bool first = true;
        while (args->Next(1, &arg, &fetched) == S_OK && fetched == 1 && arg) {
            Com<IDiaSymbol> ownarg(arg); IDiaSymbol* argtype = nullptr; arg->get_type(&argtype); Com<IDiaSymbol> owntype(argtype);
            if (!first) signature += ", "; first = false;
            signature += type_description(argtype);
        }
    }
    DWORD cc = 0; type->get_callingConvention(&cc);
    return signature + ") cc=" + std::to_string(cc);
}
bool enumerate(IDiaSymbol* global, DWORD tag, std::vector<Candidate>& compress,
               std::vector<Candidate>& process, std::vector<Candidate>& semantics, std::string& error) {
    IDiaEnumSymbols* children = nullptr;
    HRESULT hr = global->findChildren(static_cast<enum SymTagEnum>(tag), nullptr, 0, &children);
    if (hr != S_OK || !children) { error = "symbol_enumeration_failed"; return false; }
    Com<IDiaEnumSymbols> own(children); IDiaSymbol* child = nullptr; ULONG fetched = 0;
    while ((hr = children->Next(1, &child, &fetched)) == S_OK && fetched == 1 && child) {
        Com<IDiaSymbol> ownchild(child);
        std::wstring name = symbol_name(child);
        const bool single = name == L"CompressSingleImageTexture" || name.rfind(L"?CompressSingleImageTexture@@", 0) == 0;
        const bool threaded = name.find(L"ThreadedProcessImage<") != std::wstring::npos &&
            name.find(L"PerformBC7TextureCompression") != std::wstring::npos && name.find(L"JobData,8>") != std::wstring::npos;
        const bool predicate = name == L"TextureUtil_CUSTOM_IsRGBMUsageMode" ||
            name.rfind(L"?TextureUtil_CUSTOM_IsRGBMUsageMode@@", 0) == 0;
        if (!single && !threaded && !predicate) continue;
        Candidate candidate; candidate.tag = tag; candidate.name = utf8(name);
        child->get_relativeVirtualAddress(&candidate.rva); child->get_length(&candidate.length);
        candidate.signature = function_type(child, candidate.type_available);
        if (tag == SymTagPublicSymbol) {
            wchar_t readable[4096]{};
            if (UnDecorateSymbolNameW(name.c_str(), readable, _countof(readable), UNDNAME_COMPLETE))
                candidate.signature = utf8(readable);
        }
        (single ? compress : threaded ? process : semantics).push_back(std::move(candidate));
    }
    if (FAILED(hr)) { error = "symbol_enumeration_failed"; return false; }
    return true;
}
std::set<uint32_t> entries(const std::vector<Candidate>& candidates, const rgbm::PeImage& image) {
    std::set<uint32_t> result;
    for (const auto& candidate : candidates)
        if (image.function(candidate.rva).begin && image.at(candidate.rva, 32, true)) result.insert(candidate.rva);
    return result;
}
struct Edge { uint32_t instruction, target; std::string kind; };
Candidate describe_entry(IDiaSession* session, uint32_t rva) {
    for (DWORD tag : {DWORD(SymTagFunction), DWORD(SymTagPublicSymbol)}) {
        IDiaSymbol* raw = nullptr; LONG displacement = 0;
        auto hr = session->findSymbolByRVAEx(rva, static_cast<enum SymTagEnum>(tag), &raw, &displacement);
        Com<IDiaSymbol> symbol(raw);
        if (hr != S_OK || !symbol || displacement) continue;
        DWORD entry = 0; symbol->get_relativeVirtualAddress(&entry);
        if (entry != rva) continue;
        Candidate candidate; candidate.rva = rva; candidate.tag = tag;
        candidate.name = utf8(symbol_name(symbol.get())); symbol->get_length(&candidate.length);
        candidate.signature = function_type(symbol.get(), candidate.type_available);
        BSTR readable = nullptr;
        if (symbol->get_undecoratedName(&readable) == S_OK && readable) candidate.signature = utf8(readable);
        SysFreeString(readable);
        return candidate;
    }
    Candidate missing; missing.rva = rva; missing.name = "unresolved"; return missing;
}
uint32_t forwarder_target(const rgbm::PeImage& image, uint32_t entry) {
    const auto* code = image.at(entry, 16, true);
    const uint8_t abi[] = {0x45, 0x8b, 0xc8, 0x44, 0x8b, 0xc2, 0x48, 0x8b, 0xd1, 0x33, 0xc9, 0xe9};
    // A leaf callback forwarding (JobData*,begin,end) to (0,JobData*,begin,end).
    // This is a body contract, not an arbitrary function prologue fingerprint.
    if (!code || memcmp(code, abi, sizeof(abi))) return 0;
    int32_t delta = 0; memcpy(&delta, code + 12, 4);
    const int64_t target = int64_t(entry) + 16 + delta;
    return target > 0 && target <= UINT32_MAX && image.function(uint32_t(target)).begin ? uint32_t(target) : 0;
}
// Detours decodes instruction boundaries; LEA's RIP displacement is interpreted
// only for this exact opcode. No bytewise E8/LEA search through immediates/data.
std::vector<Edge> decode_edges(const rgbm::PeImage& image, rgbm::FunctionRange range) {
    std::vector<Edge> edges;
    if (!range.begin) return edges;
    const auto size = range.end - range.begin;
    const auto* data = image.at(range.begin, size, true);
    // A padded local copy prevents decoder reads past the disk function boundary.
    std::vector<uint8_t> copy(data, data + size); copy.resize(size + 32, 0);
    for (uint32_t offset = 0; offset < size;) {
        auto* instruction = copy.data() + offset;
        const auto length = static_cast<uint32_t>(rgbm::disk_instruction_length(instruction));
        if (!length || length > size - offset) break;
        const auto rva = range.begin + offset;
        int32_t displacement = 0;
        if (length == 5 && instruction[0] == 0xe8) {
            memcpy(&displacement, instruction + 1, 4);
            const int64_t target = int64_t(rva) + length + displacement;
            if (target >= 0 && target <= UINT32_MAX) edges.push_back({rva, uint32_t(target), "call"});
        } else if (length == 7 && instruction[0] == 0x4c && instruction[1] == 0x8d && instruction[2] == 0x05) {
            memcpy(&displacement, instruction + 3, 4);
            const int64_t target = int64_t(rva) + length + displacement;
            if (target >= 0 && target <= UINT32_MAX) edges.push_back({rva, uint32_t(target), "callback_r8"});
        } else if (length == 7 && instruction[0] == 0x48 && instruction[1] == 0x8d && instruction[2] == 0x15) {
            memcpy(&displacement, instruction + 3, 4);
            const int64_t target = int64_t(rva) + length + displacement;
            if (target >= 0 && target <= UINT32_MAX) edges.push_back({rva, uint32_t(target), "dispatcher_rdx"});
        }
        offset += length;
    }
    return edges;
}
std::string candidates_json(const std::vector<Candidate>& candidates, const rgbm::PeImage& image) {
    std::ostringstream out; out << '['; bool first = true;
    for (const auto& c : candidates) {
        if (!first) out << ','; first = false;
        auto range = image.function(c.rva);
        out << "{\"rva\":" << c.rva << ",\"pdb_length\":" << c.length
            << ",\"unwind_end\":" << range.end << ",\"source\":\""
            << (c.tag == SymTagFunction ? "pdb_function" : "pdb_public") << "\",\"name\":" << rgbm::json_string(c.name)
            << ",\"signature\":" << rgbm::json_string(c.signature)
            << ",\"type_available\":" << (c.type_available ? "true" : "false") << '}';
    }
    return out.str() + ']';
}
std::string hr_string(HRESULT hr) { char text[16]; sprintf_s(text, "0x%08lX", static_cast<unsigned long>(hr)); return text; }
}

int wmain(int argc, wchar_t** argv) {
    std::map<std::wstring, std::wstring> options;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::wstring key = argv[i];
        if (key != L"--exe" && key != L"--pdb" && key != L"--dia" && key != L"--report" && key != L"--cache") return 2;
        if (!options.emplace(key, argv[i + 1]).second) return 2;
    }
    if ((argc != 9 && argc != 11) || !options.count(L"--exe") || !options.count(L"--pdb") ||
        !options.count(L"--dia") || !options.count(L"--report") || options.size() != size_t((argc - 1) / 2)) {
        fwprintf(stderr, L"Usage: bc7_rgbm_resolver --exe <exe> --pdb <pdb> --dia <msdia140.dll> --report <json>\n"); return 2;
    }
    std::wstring reportpath = options[L"--report"];
    std::wstring exe_full(32768, L'\0'), report_full(32768, L'\0'), pdb_full(32768, L'\0'), dia_full(32768, L'\0');
    auto full = [](const std::wstring& path, std::wstring& result) {
        DWORD count = GetFullPathNameW(path.c_str(), static_cast<DWORD>(result.size()), result.data(), nullptr);
        if (!count || count >= result.size()) return false; result.resize(count); return true;
    };
    if (!full(options[L"--exe"], exe_full) || !full(reportpath, report_full) ||
        !full(options[L"--pdb"], pdb_full) || !full(options[L"--dia"], dia_full) ||
        !_wcsicmp(report_full.c_str(), exe_full.c_str()) || !_wcsicmp(report_full.c_str(), pdb_full.c_str()) ||
        !_wcsicmp(report_full.c_str(), dia_full.c_str())) { fprintf(stderr, "unsafe_output_path\n"); return 2; }
    std::wstring cache_full(32768, L'\0');
    if (options.count(L"--cache") && (!full(options[L"--cache"], cache_full) ||
        !_wcsicmp(cache_full.c_str(), exe_full.c_str()) || !_wcsicmp(cache_full.c_str(), pdb_full.c_str()) ||
        !_wcsicmp(cache_full.c_str(), dia_full.c_str()) || !_wcsicmp(cache_full.c_str(), report_full.c_str()))) {
        fprintf(stderr, "unsafe_output_path\n"); return 2;
    }
    rgbm::PeImage image; std::string error, failure; HRESULT pdb_status = E_FAIL;
    const bool pe_ok = image.load(exe_full, error);
    std::vector<Candidate> compress, process, semantics, dependencies, forwarders, fence_dependencies;
    std::vector<Edge> edges, process_edges, fence_edges;
    std::set<uint32_t> callbacks;
    std::wstring loaded_dia_path;
    const HRESULT com_status = pe_ok ? CoInitializeEx(nullptr, COINIT_MULTITHREADED) : E_FAIL;
    if (!pe_ok) failure = error;
    else if (FAILED(com_status)) failure = "com_initialization_failed";
    else {
        // The caller supplies a local DIA path. No hardcoded VS path or COM registration.
        LocalDiaModule dia_module;
        IDiaDataSource* rawsource = nullptr;
        HRESULT hr = dia_module.create(dia_full, &rawsource);
        loaded_dia_path = dia_module.path();
        Com<IDiaDataSource> source(rawsource);
        if (FAILED(hr) || !source) failure = "dia_load_failed:" + hr_string(hr);
        else {
            GUID expected = image.identity().pdb_guid;
            pdb_status = source->loadAndValidateDataFromPdb(pdb_full.c_str(), &expected, 0, image.identity().pdb_age);
            if (pdb_status != S_OK) failure = "pdb_identity_mismatch_or_unreadable:" + hr_string(pdb_status);
            else {
                IDiaSession* rawsession = nullptr; hr = source->openSession(&rawsession); Com<IDiaSession> session(rawsession);
                IDiaSymbol* rawglobal = nullptr;
                if (hr == S_OK && session) hr = session->get_globalScope(&rawglobal);
                Com<IDiaSymbol> global(rawglobal);
                if (hr != S_OK || !global) failure = "pdb_session_failed";
                else if (!enumerate(global.get(), SymTagFunction, compress, process, semantics, error) ||
                         !enumerate(global.get(), SymTagPublicSymbol, compress, process, semantics, error)) failure = error;
                else {
                    auto singles = entries(compress, image), processes = entries(process, image);
                    if (singles.size() != 1 || processes.size() != 1) failure = "missing_or_ambiguous_named_entry";
                    else {
                        edges = decode_edges(image, image.function(*singles.begin()));
                        process_edges = decode_edges(image, image.function(*processes.begin()));
                        for (const auto& edge : process_edges)
                            if (edge.kind == "call" || edge.kind == "dispatcher_rdx")
                                dependencies.push_back(describe_entry(session.get(), edge.target));
                        for (const auto& dependency : dependencies)
                            if (dependency.name.find("?CompleteFenceInternal@@") == 0) {
                                fence_edges = decode_edges(image, image.function(dependency.rva));
                                for (const auto& edge : fence_edges)
                                    if (edge.kind == "call")
                                        fence_dependencies.push_back(describe_entry(session.get(), edge.target));
                            }
                        for (size_t i = 1; i < edges.size(); ++i)
                            if (edges[i].kind == "call" && edges[i].target == *processes.begin() &&
                                edges[i - 1].kind == "callback_r8" &&
                                edges[i].instruction - edges[i - 1].instruction <= 32 &&
                                image.at(edges[i - 1].target, 16, true)) {
                                const auto target = forwarder_target(image, edges[i - 1].target);
                                if (!target) continue;
                                const auto nested = decode_edges(image, image.function(target));
                                for (const auto& call : nested)
                                    if (call.kind == "call" && image.imported_function(call.target) == "compress_bc7e.dll!bc7elib_compress_blocks") {
                                        callbacks.insert(target);
                                        forwarders.push_back(describe_entry(session.get(), edges[i - 1].target));
                                        break;
                                    }
                            }
                        failure = callbacks.size() == 1 ? "interface_and_rgbm_semantics_not_yet_validated" : "missing_or_ambiguous_callback";
                    }
                }
            }
        }
    }
    if (SUCCEEDED(com_status)) CoUninitialize();
    bool contract_verified = false, cache_written = false;
    if (options.count(L"--cache") && failure == "interface_and_rgbm_semantics_not_yet_validated") {
        const std::string expected = "?CompressSingleImageTexture@@YAXAEAVImageReference@@W4TextureFormat@@W4TextureUsageMode@@H_N33QEAEQEBVTexture@@@Z";
        auto signature = std::find_if(compress.begin(), compress.end(), [&](const Candidate& c) { return c.name == expected; });
        auto singles = entries(compress, image), processes = entries(process, image), predicates = entries(semantics, image);
        rgbm::ContractBindings bindings;
        if (signature == compress.end() || singles.size() != 1 || processes.size() != 1 || predicates.size() != 1)
            failure = "pdb_abi_or_semantics_missing_or_ambiguous";
        else if (!(contract_verified = rgbm::certify_contract(image, {*singles.begin(), *processes.begin(), *predicates.begin()}, bindings, error)))
            failure = "native_contract_rejected:" + error;
        else if (!(cache_written = rgbm::write_binding_cache(cache_full, bindings, error))) failure = "cache_publication_failed:" + error;
        else failure = "contract_verified_cache_published";
    }
    const auto& id = image.identity();
    std::ostringstream out;
    out << "{\n\"schema_version\":1,\"resolver_version\":\"0.3.0-evidence\",\"artifact_kind\":\"evidence_report\","
        << "\"installable\":false,\"state\":\"Unavailable\",\"reason\":" << rgbm::json_string(failure)
        << ",\"dia_loaded_path\":" << rgbm::json_string(utf8(loaded_dia_path))
        << ",\"dia_load_method\":\"private_DllGetClassObject\""
        << ",\"contract_verified\":" << (contract_verified ? "true" : "false")
        << ",\"cache_written\":" << (cache_written ? "true" : "false")
        << ",\n\"binary_identity\":{\"exe_path\":" << rgbm::json_string(utf8(exe_full))
        << ",\"sha256\":" << rgbm::json_string(id.sha256) << ",\"timestamp\":" << id.timestamp
        << ",\"image_size\":" << id.image_size << ",\"pdb_guid\":" << rgbm::json_string(rgbm::guid_string(id.pdb_guid))
        << ",\"pdb_age\":" << id.pdb_age << ",\"pdb_name\":" << rgbm::json_string(id.pdb_name)
        << ",\"pdb_validated\":" << (pdb_status == S_OK ? "true" : "false") << "},"
        << "\n\"compress_candidates\":" << candidates_json(compress, image)
        << ",\n\"process_candidates\":" << candidates_json(process, image) << ",\n\"callback_candidates\":[";
    bool first = true;
    for (auto rva : callbacks) {
        if (!first) out << ','; first = false;
        out << "{\"rva\":" << rva << ",\"unwind_end\":" << image.function(rva).end
            << ",\"source\":\"caller_forwarder_bc7_import\",\"import\":\"compress_bc7e.dll!bc7elib_compress_blocks\"}";
    }
    out << "],\n\"semantic_candidates\":" << candidates_json(semantics, image)
        << ",\n\"process_dependencies\":" << candidates_json(dependencies, image)
        << ",\n\"fence_dependencies\":" << candidates_json(fence_dependencies, image)
        << ",\n\"callback_forwarders\":" << candidates_json(forwarders, image)
        << ",\n\"call_edges\":["; first = true;
    for (const auto& edge : edges) {
        if (!first) out << ','; first = false;
        out << "{\"instruction_rva\":" << edge.instruction << ",\"target_rva\":" << edge.target
            << ",\"kind\":" << rgbm::json_string(edge.kind) << '}';
    }
    out << "],\n\"process_edges\":["; first = true;
    for (const auto& edge : process_edges) {
        if (!first) out << ','; first = false;
        out << "{\"instruction_rva\":" << edge.instruction << ",\"target_rva\":" << edge.target
            << ",\"kind\":" << rgbm::json_string(edge.kind) << '}';
    }
    out << "],\n\"fence_edges\":["; first = true;
    for (const auto& edge : fence_edges) {
        if (!first) out << ','; first = false;
        out << "{\"instruction_rva\":" << edge.instruction << ",\"target_rva\":" << edge.target
            << ",\"kind\":" << rgbm::json_string(edge.kind) << '}';
    }
    out << "]}\n";
    if (!rgbm::publish_report(report_full, out.str(), error)) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
    // 0 means evidence extraction succeeded; it NEVER means safe to install hooks.
    const bool located = failure == "interface_and_rgbm_semantics_not_yet_validated" || failure == "contract_verified_cache_published";
    fprintf(stdout, "report=%ls located=%d installable=0 reason=%s\n", report_full.c_str(), located, failure.c_str());
    return located ? 0 : 3;
}
