// 最小化版本：本仓库只编译 ert.cpp / bc7decomp.cpp，utils.h 仅需 DXGI_FORMAT
// 类型（见 utils.h 中 save_dds() 的声明），因此这里只保留类型定义。
// 完整上游文件见 https://github.com/richgel999/bc7enc_rdo/blob/master/dds_defs.h
#pragma once

typedef enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0
} DXGI_FORMAT;
