# Feature 施工记录：RGBM Hook DLL 合并

- Feature ID：`rgbm-hook-dll-consolidation`
- 更新日期：2026-10-10（Asia/Shanghai）
- 状态：TODO，未开始施工
- 配套：[设计与待办](rgbm-hook-dll-consolidation-design.md)

## R01：2026-10-10 / 用户要求登记后续 feature

- 用户要求先完成 C# 脚本合并，DLL 合并先记录为 TODO。
- 已记录 coordinator 并入主 DLL、保留独立 gate 的目标、拟改动和回归关口。
- 未修改原生源码、DLL 链接或 gate 导入目标，未构建／验证 DLL 合并版本。
- C# 文件合并及验证记录归属 [portability 施工记录](rgbm-hook-portability-construction.md)，不算作本 feature 的 DLL 合并施工。

## 阶段追踪

| 阶段 | 状态 | 交付／通过条件 |
| --- | --- | --- |
| 依赖与寿命审查 | 待施工 | 明确 gate 导入、安装窗口分配及模块保留方案 |
| 原生合并 | 待施工 | coordinator 链接到主 DLL，保留完整事务与恢复语义 |
| 构建／迁移 | 待施工 | 新包不含 coordinator，安全升级和平台设置完成 |
| 回归／交付 | 待施工 | 原生故障矩阵、跨版本真实 Unity 矩阵、产物身份及恢复通过 |
