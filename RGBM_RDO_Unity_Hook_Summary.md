# Unity RGBM 与 BC7 RDO：定位、实现与验证总结

## 问题

对 Unity 导入的 RGBM HDR 贴图应用常规 BC7 RDO 时出现明显质量劣化。

根因是 RGBM 的 HDR 重建为 `RGB * M`（其中 `M` 位于 alpha），而常规 RGBA
ERT RDO 将四个分量独立比较。单纯提高 alpha 权重并不能等价于 HDR 重建误差
度量。

## 最终结论

不需要根据 `.hdr` / `.exr` 后缀维护清单，也不应仅依赖 AssetPostprocessor。

在 **Unity 2022.3.62f1** 中，原生函数
`CompressSingleImageTexture(ImageReference&, TextureFormat, TextureUsageMode, ...)`
的第三个参数就是 `TextureUsageMode`。它是为 BC7 RDO 建立 RGBM 上下文的可靠
入口。

`TextureUtil.IsRGBMUsageMode` 是 Inspector/预览可用的查询入口，证明 Unity
确实保存了该语义；但最终压缩发生在 Job Worker，不能直接从
`bc7elib_compress_blocks()` 的参数中取得这个信息。识别结果现用于选择 ERT 的
RGBM 度量，或让 Oodle 直接处理 RGBM Job。

## 目标资源的实测结果

批处理测试资源：

```text
Assets/LMGroup_arena_4096_01_final.hdr
```

Unity 导入后的验证结果：

```text
usageMode=BakedLightmapRGBM
rgbm=True
graphicsFormat=RGBA_BC7_SRGB
```

原生 hook 日志显示：

```text
RGBMCTX compress-single RGBM usage=2 format=25 ...
RGBMCTX bc7-process ... rgbm=1 format=4096 ...
RGBMCTX bc7-job ...
RDO called (bc7enc_rdo) rgbm=1 ...
```

这证明 `TextureUsageMode=2` 被识别为 RGBM，并成功传播到 4096 mip 与后续所有
mip 的 BC7 worker；BC7encRdo 路径将使用 RGBM 重建域的度量，Oodle 路径则直接
调用 Oodle RDO。

## 最终实现链路

```text
CompressSingleImageTexture(TextureUsageMode)
    -> ThreadedProcessImage<PerformBC7TextureCompression::JobData, 8>
        -> PerformBC7TextureCompression Job lambda
            -> bc7elib_compress_blocks hook
```

实现方式：

1. hook `CompressSingleImageTexture`，用 Unity 内部同等逻辑识别 RGBM usage
   （该版本为 bit 2 / 5 / 9）。
2. 在该函数的同步压缩范围内建立调用线程上下文。
3. hook BC7 Job 调度点，把上下文关联到实际 Job 的源像素。
4. hook BC7 Job lambda，在 Unity worker 线程恢复 `thread_local` RGBM 上下文。
5. `bc7elib_compress_blocks` 发现 RGBM 后，`encoder=bc7enc_rdo` 使用 RGBM
   重建域的 ERT 度量；`encoder=oodle` 直接调用 Oodle RDO。

目标格式是 `RGBA_BC7_SRGB`，所以 ERT 的 RGBM 度量比较每个候选 BC7 块解码后
与源图的 `Linear(sRGB) * M` 三通道 MSE，并以同一重建域计算光滑块标准差和
RMS 限制。它保持普通 8-bit MSE 数值
尺度，所以现有 `lambda`、`max_rms_increase_ratio` 等档位仍可直接使用。普通
RGBA/非 RGBM BC7 不受影响，也不会因为中间图像复制或像素地址复用错误匹配 RGBM。

## 已排除的方案

最初尝试从 `ConvertImageToRGBM` 的 `ImageReference::pixels` 地址向 BC7 Job
传播标记。批处理日志表明 RGBM 转换缓冲区和实际 BC7 输入缓冲区并不相同，例如：

```text
RGBM 转换像素：0x...9280001030
BC7 4096 输入：0x...9260001030
```

即使再 hook `ConvertImageForCompressionIfNeeded`，该中间路径也无法可靠保持
标记。因此最终改为使用 `CompressSingleImageTexture` 的原生
`TextureUsageMode` 参数，而非推断像素来源。

## 配置

项目插件目录当前配置：

```ini
encoder=oodle
lambda=1
lookback=128
smooth_scale=15
max_rms_increase_ratio=10
two_matches=1
oodle_effort=high
oodle_lambda=30
rgbm_oodle_lambda=30
debug=0
trace_bc7_callstacks=0
unity_rgbm_context_hooks=1
rgbm_rdo=1
rgbm_lambda=0.25
rgbm_smooth_scale=30
rgbm_max_rms_increase_ratio=1.5
```

配置含义：

- `unity_rgbm_context_hooks=1`：在 Unity 进程启动时安装 RGBM 上下文 hook。
  此项需要重启 Unity 才能生效。
- `rgbm_rdo=1`：仅对 Unity 已标记为 RGBM 的 BC7 Job，在
  `encoder=bc7enc_rdo` 时使用 `Linear(sRGB) * M` HDR 重建域的 ERT 度量。
- `rgbm_lambda=0.25`、`rgbm_smooth_scale=30`、
  `rgbm_max_rms_increase_ratio=1.5`：使用独立且偏质量优先的 RGBM 参数；全局
  `lambda=0` 仍会关闭全部 ERT。
- `encoder=oodle`：RGBM 与普通 BC7 都直接走 Oodle RDO；不使用 ERT 的
  `rgbm_rdo` 参数。`rgbm_oodle_lambda` 可只调整 RGBM 的 Oodle RDO 强度；
  设为 `-1` 时继承通用 `oodle_lambda`。
- `debug=0`：关闭逐 BC7 调用日志。调试时可设为 `1`，会输出
  `RDO called (... ) rgbm=...`。

## 版本与安全约束

- 当前原生地址和函数 prologue 指纹只针对 Unity **2022.3.62f1** x64。
- hook 安装前会校验固定入口指纹；不匹配时拒绝安装，避免对其他 Unity 版本
  直接 patch。
- Oodle Texture 2.9.16 的可用 RDO metric 仍只有 Default / RMSE RGBA /
  Perceptual RGBA，不含显式 RGBM metric；按实测，RGBM 直接走 Oodle 没有明显
  劣化，因此不会退回或跳过其 RDO。需要严格 RGBM 重建误差时，使用
  `encoder=bc7enc_rdo` 与 `rgbm_rdo=1`。

## 相关实现文件

- `src/bc7_hook_dll.cpp`：Unity RGBM 上下文 hook、BC7 worker 上下文传递、
  BC7encRdo/Oodle 的 RGBM 分流。
- `src/rdo_common.h`：`unity_rgbm_context_hooks` 和 `rgbm_rdo` 配置解析。
- `vendor/bc7enc_rdo/ert.{h,cpp}`：RGBM 重建域误差、光滑块和 RMS 判断。
- `src/editor_plugin/README.md`：插件配置说明。
- `tools/symbolize_unity.cpp`：使用 Unity 随附 PDB/DIA 解析 Unity RVA 的定位工具。
- `Assets/Editor/RgbmUsageProbe.cs`（目标 Unity 项目）：批处理与 Editor 中
  验证 `TextureUsageMode` / RGBM 状态的工具。
