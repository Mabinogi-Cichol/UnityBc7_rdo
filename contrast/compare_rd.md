# BC7 RDO 对比：原版（ERT λ=1.0） vs Oodle（2.9.16 λ=30）

图片：`contrast/image/`；BC7 块流统一用壳 DLL 的 `bc7elib_debug_unpack_blocks` 解码，PSNR 用同一 MSE 公式，压缩体积用 zlib(9)。

## 平均值汇总（11 张图）

| 版本 | λ | 平均 zlib9 字节 | 平均节省 | 平均 PSNR(RGB) dB | 平均 PSNR(RGBA) dB |
| --- | ---: | ---: | ---: | ---: | ---: |
| 原版 | 1.0 | 2,541,675 | +53.7% | 41.88 | 42.75 |
| Oodle | 30 | 2,947,114 | +50.0% | 47.28 | 48.01 |

基线（λ=0，不应用 RDO）对比：

| 版本 | 平均 zlib9 字节 | 平均 PSNR(RGB) dB | 平均 PSNR(RGBA) dB |
| --- | ---: | ---: | ---: |
| 原版 | 5,483,884 | 47.69 | 48.54 |
| Oodle | 5,890,123 | 50.50 | 51.01 |

| 图片 | 版本 | λ | zlib9 字节 | 相对基线 | PSNR(RGB) dB | PSNR(RGBA) dB |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| beta_fullbody_default | 原版 | 1.0 | 1,401,575 | +47.8% | 44.78 | 45.67 |
| beta_fullbody_default | 原版(基线) | 0 | 2,683,669 | — | 49.64 | 50.53 |
| beta_fullbody_default | Oodle | 30 | 1,440,263 | +50.3% | 50.07 | 50.67 |
| beta_fullbody_default | Oodle(基线) | 0 | 2,896,463 | — | 52.83 | 53.10 |
| beta_fullbody_hsdhy | 原版 | 1.0 | 2,806,311 | +54.0% | 41.94 | 42.72 |
| beta_fullbody_hsdhy | 原版(基线) | 0 | 6,096,398 | — | 47.55 | 48.53 |
| beta_fullbody_hsdhy | Oodle | 30 | 3,248,346 | +50.8% | 47.61 | 48.37 |
| beta_fullbody_hsdhy | Oodle(基线) | 0 | 6,599,588 | — | 50.40 | 51.18 |
| beta_fullbody_yddcj | 原版 | 1.0 | 3,512,698 | +45.3% | 41.48 | 42.42 |
| beta_fullbody_yddcj | 原版(基线) | 0 | 6,423,413 | — | 46.99 | 47.93 |
| beta_fullbody_yddcj | Oodle | 30 | 3,688,336 | +45.6% | 46.10 | 46.98 |
| beta_fullbody_yddcj | Oodle(基线) | 0 | 6,782,756 | — | 49.60 | 50.24 |
| beta_painting | 原版 | 1.0 | 1,078,188 | +47.1% | 39.83 | 40.81 |
| beta_painting | 原版(基线) | 0 | 2,039,885 | — | 45.03 | 45.86 |
| beta_painting | Oodle | 30 | 1,017,023 | +54.1% | 45.00 | 45.60 |
| beta_painting | Oodle(基线) | 0 | 2,216,268 | — | 47.87 | 48.10 |
| cherub_fullbody_10502 | 原版 | 1.0 | 2,760,272 | +54.5% | 42.06 | 42.75 |
| cherub_fullbody_10502 | 原版(基线) | 0 | 6,066,962 | — | 48.98 | 49.76 |
| cherub_fullbody_10502 | Oodle | 30 | 3,200,979 | +51.0% | 48.14 | 48.75 |
| cherub_fullbody_10502 | Oodle(基线) | 0 | 6,533,021 | — | 51.40 | 51.95 |
| cherub_fullbody_default | 原版 | 1.0 | 2,321,368 | +53.2% | 42.58 | 43.59 |
| cherub_fullbody_default | 原版(基线) | 0 | 4,960,271 | — | 49.74 | 50.42 |
| cherub_fullbody_default | Oodle | 30 | 2,667,072 | +50.8% | 49.10 | 49.71 |
| cherub_fullbody_default | Oodle(基线) | 0 | 5,416,058 | — | 52.59 | 52.74 |
| cherub_painting | 原版 | 1.0 | 1,062,027 | +53.4% | 40.36 | 41.37 |
| cherub_painting | 原版(基线) | 0 | 2,277,352 | — | 47.21 | 47.89 |
| cherub_painting | Oodle | 30 | 1,053,478 | +57.7% | 46.02 | 46.71 |
| cherub_painting | Oodle(基线) | 0 | 2,492,742 | — | 49.90 | 50.10 |
| skysaber_fullbody_12503 | 原版 | 1.0 | 3,164,562 | +49.4% | 42.61 | 43.17 |
| skysaber_fullbody_12503 | 原版(基线) | 0 | 6,255,667 | — | 48.46 | 49.36 |
| skysaber_fullbody_12503 | Oodle | 30 | 3,626,645 | +45.0% | 47.86 | 48.74 |
| skysaber_fullbody_12503 | Oodle(基线) | 0 | 6,594,499 | — | 51.35 | 51.98 |
| skysaber_fullbody_12504 | 原版 | 1.0 | 3,688,153 | +57.5% | 41.50 | 42.43 |
| skysaber_fullbody_12504 | 原版(基线) | 0 | 8,686,861 | — | 46.66 | 47.51 |
| skysaber_fullbody_12504 | Oodle | 30 | 4,702,838 | +49.9% | 46.85 | 47.69 |
| skysaber_fullbody_12504 | Oodle(基线) | 0 | 9,380,028 | — | 49.50 | 50.19 |
| skysaber_fullbody_default | 原版 | 1.0 | 2,426,686 | +49.8% | 41.70 | 42.71 |
| skysaber_fullbody_default | 原版(基线) | 0 | 4,835,127 | — | 45.38 | 46.41 |
| skysaber_fullbody_default | Oodle | 30 | 2,744,504 | +46.8% | 45.93 | 46.75 |
| skysaber_fullbody_default | Oodle(基线) | 0 | 5,155,587 | — | 48.11 | 48.74 |
| skysaber_fullbody_kpwl | 原版 | 1.0 | 3,736,583 | +62.6% | 41.87 | 42.57 |
| skysaber_fullbody_kpwl | 原版(基线) | 0 | 9,997,117 | — | 49.00 | 49.75 |
| skysaber_fullbody_kpwl | Oodle | 30 | 5,028,774 | +53.1% | 47.41 | 48.20 |
| skysaber_fullbody_kpwl | Oodle(基线) | 0 | 10,724,348 | — | 52.00 | 52.77 |
