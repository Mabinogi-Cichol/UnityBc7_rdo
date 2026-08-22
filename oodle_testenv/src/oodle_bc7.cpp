// oodle_bc7.cpp
// BC7 RDO sweep harness for the OFFICIAL precompiled Oodle Texture 2.9.16 DLL.
//
// Loads oo2tex_win64_2.9.16.dll at runtime via LoadLibraryW/GetProcAddress
// (no .lib linking). Generates a deterministic procedural RGBA8 test image,
// runs a lambda sweep of OodleTex_EncodeBCN_RDO_Ex (lambda 0 uses the
// baseline OodleTex_EncodeBCN_Blocks), decodes back, measures RMSE with
// OodleTex_RMSE_Normalized_BCNAware, and writes per-lambda BC7 files plus
// a deterministic sweep.csv.
//
// CLI:
//   oodle_bc7.exe sweep <outdir> --size 1024 [--lambdas 0,1,5,10,20,30,40,60,100]

// _CRT_SECURE_NO_WARNINGS and NOMINMAX are supplied via the build command line.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#include "oodle2tex.h"

//---------------------------------------------------------------------------
// Function-pointer typedefs mirroring oodle2tex.h (all __stdcall, undecorated).
//---------------------------------------------------------------------------
typedef OodleTex_Err (OOEXPLINK *pfn_LogVersion)(void);
typedef const char* (OOEXPLINK *pfn_Err_GetName)(OodleTex_Err e);
typedef OO_S32 (OOEXPLINK *pfn_BC_BytesPerBlock)(OodleTex_BC bcn);
typedef OodleTex_Err (OOEXPLINK *pfn_EncodeBCN_Blocks)(
    OodleTex_BC to_bcn, void* to_bcn_blocks, OO_SINTa num_blocks,
    const void* from_pixel_blocks, OodleTex_PixelFormat from_format,
    OodleTex_EncodeEffortLevel level, OodleTex_BCNFlags flags,
    int num_job_threads, void* jobify_user_ptr);
typedef OodleTex_Err (OOEXPLINK *pfn_EncodeBCN_RDO_Ex)(
    OodleTex_BC to_bcn, void* to_bcn_blocks, OO_SINTa num_blocks,
    const OodleTex_Surface* from_surfaces, OO_SINTa num_from_surfaces,
    OodleTex_PixelFormat from_format, const OodleTex_Layout* layout,
    int rdo_lagrange_lambda, const OodleTex_RDO_Options* options,
    int num_job_threads, void* jobify_user_ptr);
typedef OodleTex_Err (OOEXPLINK *pfn_DecodeBCN_Blocks)(
    void* to_pixel_blocks, OodleTex_PixelFormat to_format, OO_SINTa num_blocks,
    OodleTex_BC from_bcn, const void* from_bcn_blocks);
typedef OO_F32 (OOEXPLINK *pfn_RMSE_Normalized_BCNAware)(
    OodleTex_BC was_bcn,
    const OodleTex_Surface* surf1, OodleTex_PixelFormat format1,
    const OodleTex_Surface* surf2, OodleTex_PixelFormat format2);
typedef t_fp_OodleTex_Plugin_Printf* (OOEXPLINK *pfn_Plugins_SetPrintf)(t_fp_OodleTex_Plugin_Printf*);

//---------------------------------------------------------------------------
// Resolved entry points (assigned at runtime; names kept distinct from the
// header's function declarations).
//---------------------------------------------------------------------------
static pfn_LogVersion              g_OodleTex_LogVersion = nullptr;
static pfn_Err_GetName             g_OodleTex_Err_GetName = nullptr;
static pfn_BC_BytesPerBlock        g_OodleTex_BC_BytesPerBlock = nullptr;
static pfn_EncodeBCN_Blocks        g_OodleTex_EncodeBCN_Blocks = nullptr;
static pfn_EncodeBCN_RDO_Ex        g_OodleTex_EncodeBCN_RDO_Ex = nullptr;
static pfn_DecodeBCN_Blocks        g_OodleTex_DecodeBCN_Blocks = nullptr;
static pfn_RMSE_Normalized_BCNAware g_OodleTex_RMSE_Normalized_BCNAware = nullptr;
static pfn_Plugins_SetPrintf       g_OodleTex_Plugins_SetPrintf = nullptr;

// Forward Oodle's printf-style logging to stdout so the runtime version
// string printed by OodleTex_LogVersion is visible on the console.
static void OOEXPLINK OodlePrintf(int verboseLevel, const char* file, int line, const char* fmt, ...)
{
    (void)verboseLevel;
    va_list args;
    va_start(args, fmt);
    if (file) fprintf(stdout, "%s:%d: ", file, line);
    vfprintf(stdout, fmt, args);
    fprintf(stdout, "\n");
    va_end(args);
}

//---------------------------------------------------------------------------
// Deterministic procedural image generation (pure math, no rand()).
//---------------------------------------------------------------------------
static inline OO_U8 Clamp255(double v)
{
    if (v < 0.0) v = 0.0;
    if (v > 255.0) v = 255.0;
    return (OO_U8)(v + 0.5);
}

static void MakeTestImage(std::vector<OO_U8>& img, int size)
{
    const double TWO_PI = 6.283185307179586476925286766559;
    img.resize((size_t)size * size * 4);

    struct Blob { double cx, cy, rad, r, g, b; };
    const Blob blobs[] = {
        { 0.25, 0.30, 0.20, 0.00, 0.00, 0.55 },
        { 0.72, 0.65, 0.25, 0.55, 0.10, 0.00 },
        { 0.55, 0.18, 0.13, 0.00, 0.45, 0.10 },
        { 0.88, 0.82, 0.18, 0.30, 0.00, 0.40 },
    };
    const int nBlobs = (int)(sizeof(blobs) / sizeof(blobs[0]));

    const int ckRegion = size / 4;      // top-left checkerboard region
    const double ckCell = 8.0;          // checker cell size in pixels
    const double s = (double)(size - 1);

    for (int y = 0; y < size; ++y)
    {
        const double v = y / s;
        for (int x = 0; x < size; ++x)
        {
            const double u = x / s;

            // smooth vertical + horizontal gradients
            double r = 0.5 + 0.5 * std::sin(TWO_PI * 1.30 * u);
            double g = 0.5 + 0.5 * std::cos(TWO_PI * 1.10 * v);
            double b = 0.5 * (u + v);

            // a few large radial blobs
            for (int i = 0; i < nBlobs; ++i)
            {
                const Blob& bl = blobs[i];
                double dx = u - bl.cx, dy = v - bl.cy;
                double d = std::sqrt(dx * dx + dy * dy) / bl.rad;
                if (d < 1.0)
                {
                    double w = 1.0 - d;
                    w *= w;
                    r += bl.r * w;
                    g += bl.g * w;
                    b += bl.b * w;
                }
            }

            // high-frequency deterministic detail
            r += 0.12 * std::sin(TWO_PI * (32.0 * u + 7.0 * v));
            g += 0.10 * std::cos(TWO_PI * (29.0 * v - 5.0 * u));
            b += 0.14 * std::sin(TWO_PI * (37.0 * u + 3.0 * v)) * std::cos(TWO_PI * 31.0 * v);

            // small checkerboard region (noisy patch for RDO to work on)
            if (x < ckRegion && y < ckRegion)
            {
                const double chk = (((int)(x / ckCell) + (int)(y / ckCell)) & 1) ? 0.15 : -0.15;
                r += chk; g += chk; b += chk;
            }

            const size_t idx = ((size_t)y * size + x) * 4;
            img[idx + 0] = Clamp255(r * 255.0);
            img[idx + 1] = Clamp255(g * 255.0);
            img[idx + 2] = Clamp255(b * 255.0);
            img[idx + 3] = 255; // opaque
        }
    }
}

//---------------------------------------------------------------------------
// Layout conversions.
// OodleTex_EncodeBCN_Blocks / DecodeBCN_Blocks operate on pixels in 4x4
// block order, whereas EncodeBCN_RDO_Ex takes linear row-major surfaces.
//---------------------------------------------------------------------------
static void LinearToBlockOrder(const std::vector<OO_U8>& lin, int w, int h, std::vector<OO_U8>& blk)
{
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    blk.resize((size_t)bw * bh * 64);
    for (int by = 0; by < bh; ++by)
    {
        for (int bx = 0; bx < bw; ++bx)
        {
            for (int py = 0; py < 4; ++py)
            {
                int sy = by * 4 + py;
                if (sy >= h) sy = h - 1; // pad edge by duplicating
                for (int px = 0; px < 4; ++px)
                {
                    int sx = bx * 4 + px;
                    if (sx >= w) sx = w - 1;
                    const size_t src = ((size_t)sy * w + sx) * 4;
                    const size_t dst = ((size_t)(by * bw + bx) * 16 + py * 4 + px) * 4;
                    blk[dst + 0] = lin[src + 0];
                    blk[dst + 1] = lin[src + 1];
                    blk[dst + 2] = lin[src + 2];
                    blk[dst + 3] = lin[src + 3];
                }
            }
        }
    }
}

static void BlockToLinear(const std::vector<OO_U8>& blk, int w, int h, std::vector<OO_U8>& lin)
{
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    lin.assign((size_t)w * h * 4, 0);
    for (int by = 0; by < bh; ++by)
    {
        for (int bx = 0; bx < bw; ++bx)
        {
            for (int py = 0; py < 4; ++py)
            {
                const int sy = by * 4 + py;
                if (sy >= h) continue;
                for (int px = 0; px < 4; ++px)
                {
                    const int sx = bx * 4 + px;
                    if (sx >= w) continue;
                    const size_t src = ((size_t)(by * bw + bx) * 16 + py * 4 + px) * 4;
                    const size_t dst = ((size_t)sy * w + sx) * 4;
                    lin[dst + 0] = blk[src + 0];
                    lin[dst + 1] = blk[src + 1];
                    lin[dst + 2] = blk[src + 2];
                    lin[dst + 3] = blk[src + 3];
                }
            }
        }
    }
}

//---------------------------------------------------------------------------
static bool LoadRawImage(const std::string& path, int width, int height, std::vector<OO_U8>& img)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        fprintf(stderr, "cannot open image file: %s\n", path.c_str());
        return false;
    }
    const size_t nbytes = (size_t)width * height * 4;
    img.resize(nbytes);
    if (!f.read((char*)img.data(), (std::streamsize)nbytes))
    {
        fprintf(stderr, "image file too small (%s): expected %zu bytes (RGBA8)\n",
                path.c_str(), nbytes);
        return false;
    }
    return true;
}

static void Usage()
{
    fprintf(stderr,
        "usage: oodle_bc7.exe sweep <outdir> [--size N | --image <raw> --width W --height H] "
        "[--lambdas a,b,c,...]\n"
        "  --size     square procedural test image size in pixels (default 1024)\n"
        "  --image    raw RGBA8 input file (row-major, w*h*4 bytes); use with --width/--height\n"
        "  --width    image width in pixels (required with --image)\n"
        "  --height   image height in pixels (required with --image)\n"
        "  --lambdas  comma-separated RDO lambdas (default 0,1,5,10,20,30,40,60,100)\n");
}

static bool LoadOodle()
{
    wchar_t exePath[MAX_PATH] = { 0 };
    const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        fprintf(stderr, "FATAL: cannot determine exe path\n");
        return false;
    }
    std::wstring exeDir(exePath);
    const size_t slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exeDir = exeDir.substr(0, slash);

    // Let the loader find the DLL next to the exe, then load it explicitly.
    SetDllDirectoryW(exeDir.c_str());
    const std::wstring dllPath = exeDir + L"\\oo2tex_win64_2.9.16.dll";
    HMODULE hDll = LoadLibraryW(dllPath.c_str());
    if (!hDll)
    {
        fprintf(stderr, "FATAL: LoadLibrary failed for %ls (GetLastError=%lu)\n",
            dllPath.c_str(), (unsigned long)GetLastError());
        return false;
    }

    g_OodleTex_LogVersion               = (pfn_LogVersion)GetProcAddress(hDll, "OodleTex_LogVersion");
    g_OodleTex_Err_GetName              = (pfn_Err_GetName)GetProcAddress(hDll, "OodleTex_Err_GetName");
    g_OodleTex_BC_BytesPerBlock         = (pfn_BC_BytesPerBlock)GetProcAddress(hDll, "OodleTex_BC_BytesPerBlock");
    g_OodleTex_EncodeBCN_Blocks         = (pfn_EncodeBCN_Blocks)GetProcAddress(hDll, "OodleTex_EncodeBCN_Blocks");
    g_OodleTex_EncodeBCN_RDO_Ex         = (pfn_EncodeBCN_RDO_Ex)GetProcAddress(hDll, "OodleTex_EncodeBCN_RDO_Ex");
    g_OodleTex_DecodeBCN_Blocks         = (pfn_DecodeBCN_Blocks)GetProcAddress(hDll, "OodleTex_DecodeBCN_Blocks");
    g_OodleTex_RMSE_Normalized_BCNAware = (pfn_RMSE_Normalized_BCNAware)GetProcAddress(hDll, "OodleTex_RMSE_Normalized_BCNAware");
    g_OodleTex_Plugins_SetPrintf        = (pfn_Plugins_SetPrintf)GetProcAddress(hDll, "OodleTex_Plugins_SetPrintf");

    if (!g_OodleTex_LogVersion || !g_OodleTex_Err_GetName || !g_OodleTex_BC_BytesPerBlock ||
        !g_OodleTex_EncodeBCN_Blocks || !g_OodleTex_EncodeBCN_RDO_Ex ||
        !g_OodleTex_DecodeBCN_Blocks || !g_OodleTex_RMSE_Normalized_BCNAware)
    {
        fprintf(stderr, "FATAL: one or more Oodle Texture exports could not be resolved\n");
        return false;
    }
    if (g_OodleTex_Plugins_SetPrintf)
        g_OodleTex_Plugins_SetPrintf(&OodlePrintf);
    return true;
}

static int RunSweep(const std::string& outdir, int width, int height,
                    const std::vector<int>& lambdas, const std::string& imagePath)
{
    CreateDirectoryA(outdir.c_str(), nullptr);

    std::vector<OO_U8> lin;
    if (!imagePath.empty())
    {
        if (!LoadRawImage(imagePath, width, height, lin))
            return 1;
    }
    else
    {
        MakeTestImage(lin, width);
    }

    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    const OO_SINTa num_blocks = (OO_SINTa)bw * bh;

    // Block-order copy of the source (required input layout for EncodeBCN_Blocks).
    std::vector<OO_U8> linBlocks;
    LinearToBlockOrder(lin, width, height, linBlocks);

    OodleTex_Surface srcSurf = {};
    srcSurf.pixels = lin.data();
    srcSurf.rowStrideBytes = (OO_SINTa)width * 4;
    srcSurf.width = width;
    srcSurf.height = height;

    OodleTex_Err err = g_OodleTex_LogVersion();
    if (err != OodleTex_Err_OK)
    {
        fprintf(stderr, "OodleTex_LogVersion failed: %s\n",
            g_OodleTex_Err_GetName ? g_OodleTex_Err_GetName(err) : "?");
        return 1;
    }

    const OO_S32 bppb = g_OodleTex_BC_BytesPerBlock(OodleTex_BC7RGBA);
    if (bppb != 16)
    {
        fprintf(stderr, "unexpected BC7 bytes-per-block: %d\n", (int)bppb);
        return 1;
    }

    std::vector<OO_U8> bc7((size_t)num_blocks * 16);          // BC7 = 16 bytes/block
    std::vector<OO_U8> decBlocks((size_t)num_blocks * 64);    // decoded RGBA in block order
    std::vector<OO_U8> decLin;                                // decoded RGBA in linear order

    const std::string csvPath = outdir + "\\sweep.csv";
    std::ofstream csv(csvPath, std::ios::out | std::ios::trunc);
    if (!csv)
    {
        fprintf(stderr, "cannot open %s\n", csvPath.c_str());
        return 1;
    }
    csv << "lambda,blocks,bc7_bytes,rmse\n";

    for (int lambda : lambdas)
    {
        // ---- encode ----
        if (lambda == 0)
        {
            // baseline non-RDO encode (pixel input must be in 4x4 block order)
            err = g_OodleTex_EncodeBCN_Blocks(
                OodleTex_BC7RGBA, bc7.data(), num_blocks,
                linBlocks.data(), OodleTex_PixelFormat_4_U8_RGBA,
                OodleTex_EncodeEffortLevel_High, OodleTex_BCNFlags_None,
                OODLETEX_JOBS_DISABLE, nullptr);
        }
        else
        {
            OodleTex_RDO_Options options = {0}; // zero-init: uninitialized padding corrupts output
            options.effort = OodleTex_EncodeEffortLevel_High;
            options.metric = OodleTex_RDO_ErrorMetric_Default;
            options.universal_tiling = OodleTex_RDO_UniversalTiling_Disable;
            options.bcn_flags = OodleTex_BCNFlags_None;
            options.rdo_flags = OodleTex_RDO_Flags_None;

            err = g_OodleTex_EncodeBCN_RDO_Ex(
                OodleTex_BC7RGBA, bc7.data(), num_blocks,
                &srcSurf, 1, OodleTex_PixelFormat_4_U8_RGBA,
                nullptr, lambda, &options,
                OODLETEX_JOBS_DISABLE, nullptr);
        }
        if (err != OodleTex_Err_OK)
        {
            fprintf(stderr, "encode failed at lambda=%d: %s\n",
                lambda, g_OodleTex_Err_GetName(err));
            return 1;
        }

        // ---- decode back to RGBA (block order) ----
        err = g_OodleTex_DecodeBCN_Blocks(
            decBlocks.data(), OodleTex_PixelFormat_4_U8_RGBA, num_blocks,
            OodleTex_BC7RGBA, bc7.data());
        if (err != OodleTex_Err_OK)
        {
            fprintf(stderr, "decode failed at lambda=%d: %s\n",
                lambda, g_OodleTex_Err_GetName(err));
            return 1;
        }

        BlockToLinear(decBlocks, width, height, decLin);

        OodleTex_Surface decSurf = {};
        decSurf.pixels = decLin.data();
        decSurf.rowStrideBytes = (OO_SINTa)width * 4;
        decSurf.width = width;
        decSurf.height = height;

        // per-pixel U8-scaled RMSE, BC7RGBA-aware
        const OO_F32 rmse = g_OodleTex_RMSE_Normalized_BCNAware(
            OodleTex_BC7RGBA,
            &srcSurf, OodleTex_PixelFormat_4_U8_RGBA,
            &decSurf, OodleTex_PixelFormat_4_U8_RGBA);
        if (rmse < 0.0f)
        {
            fprintf(stderr, "RMSE returned error at lambda=%d: %f\n",
                lambda, (double)rmse);
            return 1;
        }

        // ---- write BC7 blocks ----
        const std::string binPath = outdir + "\\bc7_l" + std::to_string(lambda) + ".bin";
        {
            std::ofstream f(binPath, std::ios::binary | std::ios::trunc);
            if (!f || !f.write((const char*)bc7.data(), (std::streamsize)bc7.size()))
            {
                fprintf(stderr, "cannot write %s\n", binPath.c_str());
                return 1;
            }
        }

        csv << lambda << "," << num_blocks << "," << bc7.size() << "," << rmse << "\n";

        printf("lambda=%d blocks=%lld bc7_bytes=%zu rmse=%.6f\n",
            lambda, (long long)num_blocks, bc7.size(), (double)rmse);
    }

    csv.close();
    printf("wrote %s\n", csvPath.c_str());
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3 || std::string(argv[1]) != "sweep")
    {
        Usage();
        return 2;
    }

    std::string outdir = argv[2];
    int size = 1024;
    int width = 0, height = 0;
    std::string imagePath;
    std::vector<int> lambdas = { 0, 1, 5, 10, 20, 30, 40, 60, 100 };

    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc)
        {
            size = std::atoi(argv[++i]);
        }
        else if (a == "--image" && i + 1 < argc)
        {
            imagePath = argv[++i];
        }
        else if (a == "--width" && i + 1 < argc)
        {
            width = std::atoi(argv[++i]);
        }
        else if (a == "--height" && i + 1 < argc)
        {
            height = std::atoi(argv[++i]);
        }
        else if (a == "--lambdas" && i + 1 < argc)
        {
            lambdas.clear();
            std::istringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ','))
                lambdas.push_back(std::atoi(tok.c_str()));
        }
        else
        {
            fprintf(stderr, "unknown argument: %s\n", a.c_str());
            Usage();
            return 2;
        }
    }

    if (!imagePath.empty())
    {
        if (width <= 0 || height <= 0)
        {
            fprintf(stderr, "--image requires --width and --height\n");
            return 2;
        }
    }
    else
    {
        width = size;
        height = size;
    }

    if (width <= 0 || height <= 0 || lambdas.empty())
    {
        fprintf(stderr, "invalid size or empty lambda list\n");
        return 2;
    }

    if (!LoadOodle())
        return 1;

    printf("OODLE OodleTextureVersion (header) = %s\n", OodleTextureVersion);
    return RunSweep(outdir, width, height, lambdas, imagePath);
}
