# 2026-04-12 PSD 16-bit Gray 支持与颜色差异复盘

## 摘要

这次围绕 ZBrush alpha PSD 暴露了两层问题：

1. 第一层是“打不开”：目录 `D:\SF_ArtVault\ArtPresets\Zbrush\ZBrushes\CompressionFolds\Alphas` 下的 PSD 全部是 `16-bit / Grayscale / Raw / 1 channel`，旧代码在头部校验阶段直接拒绝 `16-bit`，导致这些文件完全不能浏览。
2. 第二层是“能打开但看起来比 Photoshop 更深”：文件本身没有嵌入 ICC profile，而 Photoshop 会按当前灰度工作空间显示 `Gray/16`；voidImageViewer 当前则把灰度样本直接降到 `8-bit` 后复制成 `RGB` 灰阶去显示，两边的灰度语义不一致，所以中间调会偏深。

结论不是“PSD 还是不支持”，而是：

- 打不开的问题已经修好。
- 颜色更深的问题属于“未标记灰度空间的显示策略缺失”，不是简单的解码失败。

## 现象

- Windows 资源管理器能看到这些文件存在，但 `voidImageViewer` 之前无法正确浏览。
- 抽样与全量头部检查后，这批文件只有两种规格：
  - `4096x4096 / Gray / 16-bit / Raw / 1 channel`
  - `1024x1024 / Gray / 16-bit / Raw / 1 channel`
- 修完解码后，图片已经能打开，但在 `voidImageViewer` 中同一张图会比 Photoshop 里更深。
- Photoshop 标签显示为 `Gray/16#/Monitor`，说明它正在按灰度工作空间解释这张图，而不只是把数值原样映射到显示器。

## 根因

### 根因 1：PSD 头部把 16-bit 直接判死

旧代码核心限制如下：

```c
if (psd->depth != 8)
{
    return 0;
}
```

这意味着只要 PSD 是 `16-bit`，即使它只是最简单的扁平灰度合成图，也会在读取最早阶段直接失败。

### 根因 2：16-bit 路径原本不存在，不能只放开头部判断

真正需要补的是整条数据链：

- 平面总字节数
- 每行字节数
- `RLE` 行解码写入位置
- `ZIP prediction` 的逐样本还原
- 逐像素取样和 alpha 读取
- 最终归一化到当前显示链路使用的 `RGBA8`

如果只删掉头部限制，不把这些位置改成按 `bytes_per_sample` 工作，结果只会是读错数据、花屏，或者潜在越界。

### 根因 3：颜色更深不是“16-bit 精度不够”，而是灰度空间语义缺失

当前 `Gray` PSD 的 16-bit 取样逻辑本质上是：

```c
return (BYTE)((value + 128) / 257);
```

随后灰度值会被直接复制到 `R/G/B` 三个通道。

这条链路只做了“数值缩位”，没有回答一个更关键的问题：

- 这个 `Gray` 数值属于哪个灰度工作空间？

而这批 ZBrush PSD 没有嵌入 ICC profile，`voidImageViewer` 也还没有“untagged grayscale policy”。因此程序现在只能把它当作普通 RGB 灰阶显示；Photoshop 则会按当前灰度工作空间去解释它，所以同一组数值会得到不同的视觉亮度。

## 处理过程

1. 先确认样本是否真的“复杂 PSD”。结果不是，文件都非常统一。
2. 抽样和全量检查头部，确认共同特征是 `16-bit grayscale raw`，而不是图层、ZIP、Duotone 或缺 merged image。
3. 定位到 `src/psd.c` 的 `depth != 8` 早退逻辑，确认这是打不开的直接根因。
4. 在 `src/psd.c` 中补齐 16-bit 合成图路径：
   - 放开目标子集的 16-bit 校验
   - 平面大小和行宽改成按 `bytes_per_sample` 计算
   - `RLE/ZIP prediction` 增加 16-bit 样本路径
   - 逐像素读取时统一走 `16-bit -> 8-bit` 取样
5. 用最小 harness 直接调用 `psd_load()` 验证，避免被 GUI 其他噪音误导。
6. 再对“为什么更深”做二次分析，确认问题落在未标记灰度空间的显示策略，而不是继续回到 PSD 容器解析层猜问题。

## 修复结果

### 已完成

- `voidImageViewer` 已可解码并显示这批 `16-bit / Grayscale / Raw` ZBrush PSD。
- `src/psd.c` 当前支持一部分常见 `16-bit` 合成图，并统一降到现有显示链路使用的 `RGBA8`。
- 已验证普通 `8-bit PSD` 不回归。

### 暂未处理

- 未嵌入 ICC 的 `Gray PSD` 与 Photoshop 的视觉一致性。
- 当前没有实现“untagged grayscale policy”。
- 因此对没有 ICC 的灰度 PSD，viewer 仍可能和 Photoshop 的显示深浅不一致。

## 验证结果

直接调用 `psd_load()` 的最小验证结果：

- `01.PSD`：`ok=1`, `4096x4096`
- `CP_SP_Cotton_01.PSD`：`ok=1`, `1024x1024`
- `C:\Users\LiuYang\Desktop\fac.psd`：`ok=1`, `1600x1600`

说明：

- 用户样本的核心解码问题已解决。
- 现有普通 PSD 路径没有被这次修改破坏。

## 经验结论

- 对 `Gray PSD`，打开成功和显示正确是两件事，不能混成一个问题讨论。
- `16-bit` 支持绝不能只改头部校验，必须按字节宽度把整条读取链一起补齐。
- 没有嵌入 ICC 的灰度文件，外部查看器和 Photoshop 是否一致，取决于是否实现了“未标记灰度空间策略”。这不是容器解析能自动解决的问题。
- Photoshop 标签里像 `Gray/16#/Monitor` 这种信息很重要，它直接说明了显示结果依赖工作空间，而不是仅依赖文件里的样本数值。

## 后续约束

- 以后如果继续处理“Gray PSD 比 Photoshop 更深”的问题，目标应该明确写成：
  - 为未嵌入 ICC 的灰度 PSD 增加 `untagged grayscale policy`
  - 至少支持 `Gray Gamma 2.2`、`Monitor Gray`、`Linear Gray` 这类可选策略
  - 不要再把问题误判为“PSD 解码还没完成”
- 在实现该策略前，相关说明里应明确：
  - `16-bit Gray PSD` 已能打开
  - 但对未标记灰度空间的文件，显示结果可能与 Photoshop 不完全一致
