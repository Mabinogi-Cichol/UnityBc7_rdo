// symbolize_unity.cpp - Resolve Unity.exe module-relative addresses using its PDB.
//
// Usage:
//   symbolize_unity.exe <Unity.exe path> <module-relative hex offset> [...]
// The tool uses DIA directly because Unity's PDB is shipped next to Unity.exe.

#include <windows.h>
#include <oleauto.h>

#include <dia2.h>
#include <diacreate.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <cwchar>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "diaguids.lib")

namespace
{

bool parse_hex(const char* text, DWORD& value)
{
    if (!text || !*text)
        return false;
    char* end = nullptr;
    const unsigned long long parsed = _strtoui64(text, &end, 16);
    if (end == text || *end != '\0' || parsed > UINT32_MAX)
        return false;
    value = static_cast<DWORD>(parsed);
    return true;
}

std::wstring to_wide(const char* text)
{
    if (!text)
        return {};
    const int count = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (!count)
        return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, &result[0], count);
    result.pop_back();
    return result;
}

std::wstring pdb_path_for(const wchar_t* unity_path)
{
    std::wstring path(unity_path ? unity_path : L"");
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L"unity_x64.pdb";
    return path.substr(0, slash + 1) + L"unity_x64.pdb";
}

void print_bstr(BSTR text)
{
    if (text)
        wprintf(L"%ls", text);
    else
        wprintf(L"<unnamed>");
}

void print_lexical_parent_chain(IDiaSymbol* symbol)
{
    IDiaSymbol* current = symbol;
    for (unsigned depth = 0; depth < 8; ++depth)
    {
        IDiaSymbol* parent = nullptr;
        if (FAILED(current->get_lexicalParent(&parent)) || !parent)
            break;

        BSTR name = nullptr;
        if (FAILED(parent->get_undecoratedName(&name)) || !name)
        {
            SysFreeString(name);
            name = nullptr;
            parent->get_name(&name);
        }
        if (name)
        {
            wprintf(L" <- ");
            print_bstr(name);
            SysFreeString(name);
        }
        else
        {
            SysFreeString(name);
        }

        if (current != symbol)
            current->Release();
        current = parent;
    }

    if (current != symbol)
        current->Release();
}

void print_source_line(IDiaSession* session, DWORD rva)
{
    IDiaEnumLineNumbers* lines = nullptr;
    if (FAILED(session->findLinesByRVA(rva, 1, &lines)) || !lines)
        return;

    IDiaLineNumber* line = nullptr;
    ULONG fetched = 0;
    if (SUCCEEDED(lines->Next(1, &line, &fetched)) && fetched == 1 && line)
    {
        IDiaSourceFile* source = nullptr;
        BSTR file_name = nullptr;
        DWORD line_number = 0;
        if (SUCCEEDED(line->get_sourceFile(&source)) && source)
            source->get_fileName(&file_name);
        line->get_lineNumber(&line_number);
        if (file_name)
            wprintf(L" (%ls:%lu)", file_name, (unsigned long)line_number);
        SysFreeString(file_name);
        if (source)
            source->Release();
        line->Release();
    }
    lines->Release();
}

void print_class_parent(IDiaSymbol* symbol)
{
    IDiaSymbol* parent = nullptr;
    if (FAILED(symbol->get_classParent(&parent)) || !parent)
        return;

    BSTR parent_name = nullptr;
    parent->get_name(&parent_name);
    ULONGLONG parent_length = 0;
    parent->get_length(&parent_length);
    wprintf(L" [class=%ls size=0x%llX]", parent_name ? parent_name : L"<unnamed>",
            static_cast<unsigned long long>(parent_length));
    SysFreeString(parent_name);

    IDiaEnumSymbols* fields = nullptr;
    if (SUCCEEDED(parent->findChildren(SymTagData, nullptr, 0, &fields)) && fields)
    {
        for (ULONG index = 0; index < 16; ++index)
        {
            IDiaSymbol* field = nullptr;
            ULONG fetched = 0;
            if (FAILED(fields->Next(1, &field, &fetched)) || fetched != 1 || !field)
                break;

            BSTR field_name = nullptr;
            LONG field_offset = 0;
            field->get_name(&field_name);
            field->get_offset(&field_offset);
            wprintf(L" [field+0x%lX=%ls]", static_cast<unsigned long>(field_offset),
                    field_name ? field_name : L"<unnamed>");
            SysFreeString(field_name);
            field->Release();
        }
        fields->Release();
    }

    parent->Release();
}

// Some Unity functions that only differ in template/lambda internals have no
// encompassing function record in the public PDB.  List nearby function
// records in that case; it still identifies the owning implementation region.
void print_nearby_functions(IDiaSession* session, DWORD rva)
{
    IDiaSymbol* global = nullptr;
    if (FAILED(session->get_globalScope(&global)) || !global)
        return;

    IDiaEnumSymbols* functions = nullptr;
    if (FAILED(global->findChildren(SymTagFunction, nullptr, 0, &functions)) || !functions)
    {
        global->Release();
        return;
    }

    const DWORD min_rva = rva > 0x3000 ? rva - 0x3000 : 0;
    const DWORD max_rva = rva + 0x1000;
    unsigned printed = 0;
    IDiaSymbol* function = nullptr;
    ULONG fetched = 0;
    while (SUCCEEDED(functions->Next(1, &function, &fetched)) && fetched == 1 && function)
    {
        DWORD entry = 0;
        ULONGLONG length = 0;
        function->get_relativeVirtualAddress(&entry);
        function->get_length(&length);
        if (entry >= min_rva && entry <= max_rva)
        {
            BSTR name = nullptr;
            if (FAILED(function->get_undecoratedName(&name)) || !name)
                function->get_name(&name);
            wprintf(L"\n  nearby [0x%X, len=0x%llX] %ls", entry,
                    static_cast<unsigned long long>(length), name ? name : L"<unnamed>");
            SysFreeString(name);
            ++printed;
        }
        function->Release();
        function = nullptr;
    }
    if (printed == 0)
        printf("\n  nearby: no public function records");
    functions->Release();
    global->Release();
}

void find_functions_by_name(IDiaSession* session, const std::wstring& needle)
{
    IDiaSymbol* global = nullptr;
    if (FAILED(session->get_globalScope(&global)) || !global)
        return;

    IDiaEnumSymbols* functions = nullptr;
    if (FAILED(global->findChildren(SymTagFunction, nullptr, 0, &functions)) || !functions)
    {
        global->Release();
        return;
    }

    ULONG fetched = 0;
    IDiaSymbol* function = nullptr;
    while (SUCCEEDED(functions->Next(1, &function, &fetched)) && fetched == 1 && function)
    {
        BSTR name = nullptr;
        if (FAILED(function->get_undecoratedName(&name)) || !name)
            function->get_name(&name);
        if (name && wcsstr(name, needle.c_str()))
        {
            DWORD entry = 0;
            ULONGLONG length = 0;
            function->get_relativeVirtualAddress(&entry);
            function->get_length(&length);
            wprintf(L"Unity.exe+0x%X len=0x%llX: %ls\n", entry,
                    static_cast<unsigned long long>(length), name);
        }
        SysFreeString(name);
        function->Release();
        function = nullptr;
    }
    functions->Release();
    global->Release();
}

void find_public_symbols_by_name(IDiaSession* session, const std::wstring& needle)
{
    IDiaSymbol* global = nullptr;
    if (FAILED(session->get_globalScope(&global)) || !global)
        return;

    IDiaEnumSymbols* symbols = nullptr;
    if (FAILED(global->findChildren(SymTagPublicSymbol, nullptr, 0, &symbols)) || !symbols)
    {
        global->Release();
        return;
    }

    ULONG fetched = 0;
    IDiaSymbol* symbol = nullptr;
    while (SUCCEEDED(symbols->Next(1, &symbol, &fetched)) && fetched == 1 && symbol)
    {
        BSTR name = nullptr;
        symbol->get_name(&name);
        if (name && wcsstr(name, needle.c_str()))
        {
            DWORD entry = 0;
            ULONGLONG length = 0;
            symbol->get_relativeVirtualAddress(&entry);
            symbol->get_length(&length);
            wprintf(L"Unity.exe+0x%X len=0x%llX: %ls\n", entry,
                    static_cast<unsigned long long>(length), name);
        }
        SysFreeString(name);
        symbol->Release();
        symbol = nullptr;
    }
    symbols->Release();
    global->Release();
}

void print_udt_layout(IDiaSession* session, const std::wstring& type_name)
{
    IDiaSymbol* global = nullptr;
    if (FAILED(session->get_globalScope(&global)) || !global)
        return;
    IDiaEnumSymbols* types = nullptr;
    if (FAILED(global->findChildren(SymTagUDT, type_name.c_str(), nsfCaseSensitive, &types)) || !types)
    {
        global->Release();
        return;
    }
    IDiaSymbol* type = nullptr;
    ULONG fetched = 0;
    while (SUCCEEDED(types->Next(1, &type, &fetched)) && fetched == 1 && type)
    {
        ULONGLONG length = 0;
        type->get_length(&length);
        wprintf(L"%ls size=0x%llX\n", type_name.c_str(), static_cast<unsigned long long>(length));
        IDiaEnumSymbols* fields = nullptr;
        if (SUCCEEDED(type->findChildren(SymTagData, nullptr, 0, &fields)) && fields)
        {
            IDiaSymbol* field = nullptr;
            ULONG field_fetched = 0;
            while (SUCCEEDED(fields->Next(1, &field, &field_fetched)) && field_fetched == 1 && field)
            {
                BSTR name = nullptr;
                LONG offset = 0;
                field->get_name(&name);
                field->get_offset(&offset);
                wprintf(L"  +0x%lX %ls\n", static_cast<unsigned long>(offset), name ? name : L"<unnamed>");
                SysFreeString(name);
                field->Release();
                field = nullptr;
            }
            fields->Release();
        }
        type->Release();
        type = nullptr;
    }
    types->Release();
    global->Release();
}

void print_symbol(IDiaSession* session, DWORD rva)
{
    IDiaSymbol* symbol = nullptr;
    long displacement = 0;
    HRESULT hr = session->findSymbolByRVAEx(rva, SymTagFunction, &symbol,
                                            &displacement);
    if (FAILED(hr) || !symbol)
    {
        printf("Unity.exe+0x%X: <unresolved, HRESULT=0x%08lX>", rva,
               static_cast<unsigned long>(hr));
        print_nearby_functions(session, rva);
        print_source_line(session, rva);
        printf("\n");
        return;
    }

    BSTR name = nullptr;
    if (FAILED(symbol->get_undecoratedName(&name)) || !name)
        symbol->get_name(&name);

    DWORD function_rva = 0;
    ULONGLONG function_length = 0;
    symbol->get_relativeVirtualAddress(&function_rva);
    symbol->get_length(&function_length);

    printf("Unity.exe+0x%X: ", rva);
    print_bstr(name);
    printf("+0x%lX [entry=0x%X length=0x%llX]",
           static_cast<unsigned long>(displacement), function_rva,
           static_cast<unsigned long long>(function_length));
    print_lexical_parent_chain(symbol);
    print_class_parent(symbol);
    print_source_line(session, rva);
    printf("\n");

    SysFreeString(name);
    symbol->Release();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s <Unity.exe path> <module-relative hex offset> [...]\n", argv[0]);
        fprintf(stderr, "       %s <Unity.exe path> --find <function-name-fragment>\n", argv[0]);
        fprintf(stderr, "       %s <Unity.exe path> --type <exact-type-name>\n", argv[0]);
        return 2;
    }

    const std::wstring unity_path = to_wide(argv[1]);
    const std::wstring pdb_path = pdb_path_for(unity_path.c_str());

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        fprintf(stderr, "CoInitializeEx failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    const wchar_t* dia_dll =
        L"C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\DIA SDK\\bin\\amd64\\msdia140.dll";
    IDiaDataSource* source = nullptr;
    hr = NoRegCoCreate(dia_dll, CLSID_DiaSource, IID_IDiaDataSource,
                       reinterpret_cast<void**>(&source));
    if (SUCCEEDED(hr))
        hr = source->loadDataFromPdb(pdb_path.c_str());

    IDiaSession* session = nullptr;
    if (SUCCEEDED(hr))
        hr = source->openSession(&session);

    if (FAILED(hr))
    {
        fprintf(stderr, "Unable to load PDB '%ls': 0x%08lX\n", pdb_path.c_str(),
                static_cast<unsigned long>(hr));
        if (source)
            source->Release();
        CoUninitialize();
        return 1;
    }

    if (strcmp(argv[2], "--find") == 0 || strcmp(argv[2], "--type") == 0)
    {
        if (argc != 4)
            fprintf(stderr, "--find requires one function-name fragment\n");
        else
        {
            if (strcmp(argv[2], "--find") == 0)
            {
                find_functions_by_name(session, to_wide(argv[3]));
                find_public_symbols_by_name(session, to_wide(argv[3]));
            }
            else
                print_udt_layout(session, to_wide(argv[3]));
        }
    }
    else for (int i = 2; i < argc; ++i)
    {
        DWORD rva = 0;
        if (!parse_hex(argv[i], rva))
        {
            fprintf(stderr, "Invalid hexadecimal offset: %s\n", argv[i]);
            continue;
        }
        print_symbol(session, rva);
    }

    session->Release();
    source->Release();
    CoUninitialize();
    return 0;
}
