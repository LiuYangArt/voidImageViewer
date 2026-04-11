# 2026-04-11 ICC 色彩管理问题复盘

## 摘要

这次 ICC 改动连续暴露了两类问题：

1. 首批启用 ICM 后，`PSD` 和部分 `JPG` 样本会崩溃。
2. 为了先止血，代码把 ICC 链路整体硬关掉，导致后续 `DisplayCAL` 测试图虽然带嵌入 ICC，但界面仍显示“NO COLORMANAGEMENT OR NOT USING EMBEDDED PROFILE”。

结论不是“测试图有问题”，而是代码先后存在“所有权错误导致崩溃”和“功能被硬禁用导致看起来没生效”这两层根因。

## 现象

- 打开 `C:\Users\LiuYang\Desktop\fac.psd` 与 `D:\SF_ArtVault\RefPictures\设计参考\武器\M4A1\500px-20131014_171117_.jpg` 时，`Debug` 版曾出现 `APPCRASH`。
- `WER / Application Error` 先落在 `icm32.dll`，后续落回 `voidImageViewer.exe`，偏移点继续漂移到 `memcpy` 附近。
- 打开 `C:\Users\LiuYang\Desktop\sRGB_Gray.png` 和 `C:\Users\LiuYang\Desktop\sRGB_Gray.jpg` 时，图片一直是红底红字，说明嵌入 profile 没有参与转换。

## 根因

### 根因 1：reply 到主线程的帧所有权转移不完整

加载线程把首帧和附加帧封进 reply 时，帧结构已经不只是 `hbitmap / mipmap / delay`，还新增了：

- `srgb_pixels`
- `display_profile_path`

但主线程消费 reply 时仍沿用旧写法，只搬运了部分字段，随后 reply 释放路径又会把 `srgb_pixels` 释放掉。这样 `_viv_frames` 里留下的就是悬空像素指针，后面一旦重建显示缓存，就会在像素复制阶段崩到 `memcpy`。

直接经验：给老结构补新字段时，不能只修“生产端”，必须同时检查：

- 队列拷贝
- 所有权转移
- reply/free 路径
- 延迟释放路径

### 根因 2：为了止血，把 ICC 功能整个硬关了

崩溃止血后，`src/color.c` 里存在如下硬闸：

```c
int color_icm_is_active(void)
{
    return 0;
}
```

而 `src/viv.c` 里所有 ICC 相关入口都以 `config_icm && color_icm_is_active()` 为条件。

这意味着：

- UI 勾选“Enable ICC color management”没有实际效果。
- 嵌入 ICC 的 `PNG / JPG / PSD / WEBP` 都不会进入源 profile 转换。
- 当前显示器 profile 也不会参与显示缓存重建。

所以 `DisplayCAL` 测试图显示为红底红字并不是“识别失败”，而是代码路径被明确禁止执行。

### 根因 3：缺少针对“颜色正确性”和“崩溃回归”的成对验证

之前验证偏向“能不能打开”，缺少这两类固定样本：

- 嵌入 ICC 后视觉结果会明显变化的样本。
- 打开后会走到显示缓存重建路径的崩溃回归样本。

结果就是：

- 崩溃修了一层，另一层悬空指针还在。
- 功能被硬关后，编译能过、图片能开，但色彩管理实际上完全没工作。

## 处理过程

1. 先从 `WER / Application Error` 里确认崩溃已经从 `icm32.dll` 转移到 `voidImageViewer.exe`。
2. 再用 `PDB` 把偏移点对回 `memcpy`，确定是我们自己的坏指针/坏长度，而不是单纯系统 ICM API 不稳定。
3. 追到 reply 消费路径，定位到帧结构新增字段后，所有权转移仍是旧写法。
4. 增加 `_viv_take_frame_ownership(...)`，把整帧一次性交给 `_viv_frames / _viv_preload_frames`，并把 reply 源结构清零，避免二次释放。
5. 重新检查 `DisplayCAL` 测试图，发现 ICC 没生效的原因不是 profile 提取失败，而是 `color_icm_is_active()` 被硬编码为 `0`。
6. 把 ICC 激活条件改成“系统 MSCMS API 齐全时启用”，并恢复 `source -> sRGB -> display` 两段真实转换。

## 最终修复

### 代码修复

- 在 `src/viv.c` 中引入 `_viv_take_frame_ownership(...)`，修复 reply 帧对象的所有权转移。
- 在 `src/color.c` 中让 `color_icm_is_active()` 基于系统 API 可用性返回真值，而不是固定返回 `0`。
- 恢复：
  - `color_transform_to_srgb(...)`
  - `color_transform_srgb_to_display(...)`

### 验证结果

- `C:\Users\LiuYang\Desktop\sRGB_Gray.png`：从红底红字变为灰底灰字。
- `C:\Users\LiuYang\Desktop\sRGB_Gray.jpg`：从红底红字变为灰底灰字。
- `C:\Users\LiuYang\Desktop\fac.psd`：重新启用 ICC 后未再新增 WER 崩溃事件，前台运行可持续存活。
- `D:\SF_ArtVault\RefPictures\设计参考\武器\M4A1\500px-20131014_171117_.jpg`：重新启用 ICC 后未再新增 WER 崩溃事件。

## 经验结论

- 崩溃止血和功能恢复必须分开跟踪。不能因为“先关功能能稳定”就把硬闸留在主线里不回头检查。
- 给核心结构体加字段时，必须把“生产、转移、释放、缓存”四条链同时过一遍，不允许只改局部。
- 色彩管理这类功能不能只验证“能打开图片”，必须验证“视觉结果确实变化”。
- 对 ICC 这种高风险路径，最少保留一组“颜色正确性样本”和一组“崩溃回归样本”。

## 后续约束

- 每次调整 `color.c / viv.c` 的 ICC 相关逻辑后，至少回归：
  - `sRGB_Gray.png`
  - `sRGB_Gray.jpg`
  - `fac.psd`
  - `500px-20131014_171117_.jpg`
- 如果再次为了稳定性临时关闭 ICC，必须同时写明：
  - 关闭原因
  - 影响范围
  - 恢复条件
  - 计划移除时间点
