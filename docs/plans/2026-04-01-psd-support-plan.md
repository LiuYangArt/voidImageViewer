# voidImageViewer PSD 支持计划

配套技术说明：

- 具体 PSD 结构、开源参考实现、解码边界和 `psd_load()` 伪代码见 [PSD 解析设计说明](2026-04-01-psd-parsing-notes.md)

## 0. 进度更新（2026-04-02）

当前仓库状态已经从“方案阶段”进入“首版已接入并可基本使用”：

- 已完成 `src/psd.c` / `src/psd.h`，并把 PSD 作为独立专用解码模块接到现有加载链路
- 已引入最小 ZIP inflate 依赖 `miniz`，用于支持 `ZIP / ZIP with prediction`
- 已接入 `.psd` 的打开文件过滤器、Everything 搜索白名单、命令行帮助、关联扩展数组、设置页关联复选框和本地化文本
- 已同步 `vs2019` / `vs2026` 工程文件，并把 `vs2026` 默认工具集改为 VS2022 可直接使用的 `v143`
- 已清理工程里残留的 `UpgradeFromVC71.props` 导入，避免 VS2022 再触发旧升级脚本错误
- 已验证 `vs2026` 工程在 VS2022 环境下 `Release|x64` / `Debug|x64` 可以成功编译
- 已完成一次手动验证：普通 PSD 文件可以成功打开

当前首版的实际边界：

- 已实现所有计划内 8-bit PSD 颜色模式的解析分发路径：`Grayscale / Indexed / RGB / CMYK / Multichannel / Duotone / Lab`
- 已实现所有计划内 8-bit PSD 压缩：`Raw / RLE / ZIP / ZIP with prediction`
- 当前显示目标仍然是“合成图只读显示”，不涉及图层编辑语义

当前仍未完成或未系统验证的部分：

- 还没有接入真正的 ICC profile 到 sRGB 颜色管理链，`CMYK / Lab / Duotone / Multichannel` 目前仍是公式型转换，精度边界低于原计划中的完整颜色管理目标
- 还没有做完整样本矩阵验证，尤其是全部颜色模式、超大 PSD、非法 PSD、16-bit PSD 的系统回归
- 还没有验证 `x86` 工程与全部历史工程配置

## 1. 结论

可以做，但范围已经从“常见 8-bit RGB/RGBA PSD”提升为“所有 8-bit PSD”。按这个目标执行时，首版定义应改为：

- 支持打开 `.psd`
- 只读显示 PSD 的扁平合成图像
- 目标覆盖所有 8-bit PSD 颜色模式：`Grayscale / Indexed / RGB / CMYK / Multichannel / Duotone / Lab`
- 覆盖所有 8-bit PSD 常见压缩：`Raw / RLE / ZIP / ZIP with prediction`
- 复用现有单帧图片显示路径，不引入图层编辑能力

这仍然不是完整 Photoshop 兼容层，因为我们依然只显示最终合成图，不实现图层编辑和 Photoshop UI 语义。但它已经明显高于原来的轻量扩展范围：除了 PSD 容器解析，还必须补齐颜色模式分发、ZIP 解压和颜色空间转换。对这个项目来说，最合理的方案仍然不是重写主显示管线，而是沿用当前 `GDI+ 失败 -> 特殊格式专用解码器` 的模式，为 PSD 增加一个单独的解码模块。

## 2. 为什么判断可行

现有代码已经具备扩展点：

- `src/viv.c` 的加载线程会先把文件读入 `IStream`。
- 常规格式使用 `GDI+` 从 `IStream` 解码。
- `WEBP` 已经是一个现成先例：当 GDI+ 失败时，会调用 `webp_load(stream, ...)` 进入专用解码路径。
- 现有回调模型已经能接收“先给尺寸和帧数，再逐帧给像素”的格式解码结果。

这意味着 PSD 完全可以按相同模式接入：

1. 在 `GDI+` 失败后调用 `psd_load(stream, ...)`
2. 将 PSD 输出转换成现有的 `HBITMAP`
3. 复用当前的缩放、预加载、显示和状态栏逻辑

从接线方式看，这仍然不需要重构渲染架构；但从技术复杂度看，这已经是高复杂度扩展，因为颜色模式、压缩和颜色管理都必须覆盖。

## 3. 推荐范围

### 首版支持

- `.psd` 扩展名识别
- 单帧 PSD
- 所有 8-bit PSD 颜色模式：`Grayscale / Indexed / RGB / CMYK / Multichannel / Duotone / Lab`
- 所有 8-bit PSD 压缩：`Raw / RLE / ZIP / ZIP with prediction`
- 8-bit RGBA / 带透明通道的 PSD
- 从 PSD 读取最终合成结果并显示
- 嵌入 ICC profile 时按 profile 转换到显示用 sRGB
- 对不支持的 PSD 给出明确失败结果，而不是崩溃或黑图

### 首版明确不做

- `.psb`
- `Bitmap` 1-bit 模式
- 16-bit / 32-bit 通道深度
- 图层面板、单图层浏览、混合模式精确复现
- 调整图层、文字图层、智能对象、滤镜效果的完整 Photoshop 级兼容
- 动画 PSD

这个边界依然重要。voidImageViewer 的定位是轻量浏览器，不是 Photoshop 渲染器；但如果目标改成“所有 8-bit PSD”，那就必须把颜色和压缩覆盖做到位。

## 4. 推荐实现方案

### 方案 A：新增内置 PSD 解码层，接入现有专用格式路径

这是推荐方案。

开始实现前，建议先阅读 [PSD 解析设计说明](2026-04-01-psd-parsing-notes.md)，再拆 `src/psd.c` 的具体函数。

做法：

- 新增 `src/psd.c` 与 `src/psd.h`
- 新增 ZIP 解压支持，优先考虑轻量依赖，例如 `miniz` 或项目可接受的等价实现
- 新增颜色空间转换能力，优先考虑引入 `Little CMS 2` 之类的成熟开源库
- 暴露与 `webp_load` 接近的接口，例如：

```c
int psd_load(
    IStream *stream,
    void *user_data,
    int (*info_callback)(void *user_data, DWORD frame_count, DWORD wide, DWORD high, int has_alpha),
    int (*frame_callback)(void *user_data, BYTE *pixels, int delay)
);
```

- 在 `src/viv.c` 的 `GDI+` 失败分支中，先尝试 `psd_load`，再决定是否最终报错
- 在 `psd_load()` 内部完成颜色模式归一化，把任意 8-bit PSD 统一转换为 `BGRA8`
- 像 `WEBP` 一样，把解码出的 `BGRA8` 像素转换为 `HBITMAP`

优点：

- 结构与现有代码一致，最容易维护
- 不依赖用户机器上是否装有 PSD codec
- 不需要引入外部进程
- 用户体验最稳定

代价：

- 需要实现或引入 PSD 容器解析、ZIP 解压和颜色管理三块能力
- 需要处理调色板、ICC、Duotone/Lab/CMYK 到显示色彩空间的转换
- 需要处理像素格式、透明度和大图内存

### 方案 B：依赖系统 codec 或 GDI+/WIC 直接打开 PSD

不推荐。

原因：

- 当前代码并没有 WIC 解码链路，主路径是 GDI+
- 即使某些机器能装第三方 PSD codec，这种支持也会变成“这台能开，那台打不开”
- 这种行为不符合项目目前“支持格式应内建且稳定”的风格

### 方案 C：调用外部工具转换 PSD 后再显示

当前不推荐。

原因：

- 需要额外安装 ImageMagick 等外部程序
- 错误处理、进程管理、临时文件、路径和安全性都会复杂化
- 这更像备用方案，不适合作为项目原生支持

## 5. 代码接入点

实现时至少会改到这些位置：

### 图片加载主链路

- `src/viv.c`
  - 在 `GDI+` 失败后，新增 `psd_load` 分支
  - 沿用现有 `first_frame` / `_viv_reply_add` / mipmap 生成流程

### 新增 PSD 解码模块

- `src/psd.c`
- `src/psd.h`

职责建议：

- 从 `IStream` 读取 PSD 数据
- 解析 header，做格式和范围判定
- 解析 `Color Mode Data`、`Image Resources`、`Image Data`
- 按颜色模式和压缩方式解码通道数据
- 输出单帧 `BGRA8` 像素
- 对不支持的特性快速失败

### 扩展名与关联

- `src/viv.c`
  - `_viv_association_extensions[]`
  - `_viv_association_description_localization_id_array`
  - `_viv_association_icon_locations`
  - `_viv_association_dlg_item_id`
  - 打开文件对话框过滤器字符串
  - Everything 搜索白名单 `ext:bmp;gif;ico;jpeg;jpg;png;tif;tiff;webp`
  - 命令行帮助中的 `/<...>` 与 `/no<...>` 文本

### 资源与本地化

- `res/resource.h`
- `res/voidImageViewer.rc`
- `src/localization.h`
- `src/localization_en_us.h`
- `src/localization_zh_cn.h`

需要新增：

- `IDC_PSD`
- `LOCALIZATION_ID_ASSOCIATION_DESCRIPTION_PSD`
- 关联对话框中的 `PSD` 复选框

### 工程文件

- `vs2019/voidImageViewer.vcxproj`
- `vs2026/voidImageViewer.vcxproj`

如果引入第三方纯 C 源码，还要把对应 `.c/.h` 加入两个工程。

## 6. 第一阶段需要先验证的事

在正式编码前，先做一个很短的技术确认阶段，先验证四件事：

1. 选定 PSD 解析方案
2. 选定 ZIP 解压和颜色管理方案
3. 验证许可证是否能并入当前项目
4. 用覆盖全部 8-bit 颜色模式的样本确认目标是否成立

技术执行细节可直接参照 [PSD 解析设计说明](2026-04-01-psd-parsing-notes.md) 的第 2 到第 16 节。

样本建议最少覆盖：

- Grayscale PSD
- Indexed PSD
- 普通 RGB PSD
- 带透明背景的 RGBA PSD
- CMYK PSD
- Lab PSD
- Duotone PSD
- Multichannel PSD
- 大尺寸 PSD
- 一个明确不支持的 16-bit PSD

通过标准：

- 所有 8-bit 颜色模式至少能稳定读出合成图
- 不支持样本能安全失败，不崩溃

## 7. 分阶段任务

## 阶段 1：选型与样本验证

当前状态：

- 部分完成。实际实现采用了“内置 PSD 解析 + `miniz` ZIP 解压”的路线。
- 颜色管理库没有在首版接入，改为先落地公式型颜色转换。
- 样本验证目前只完成了“可打开普通 PSD”的基础人工验证，尚未完成完整覆盖矩阵。

- 确定 PSD 解析方案，优先纯 C、可静态编入、依赖少
- 确定 ZIP 解压方案
- 确定颜色管理方案，重点解决 `CMYK / Lab / Duotone / Multichannel`
- 检查许可证是否与项目分发方式兼容
- 准备覆盖全部 8-bit 颜色模式的测试样本集
- 先做一个独立 spike，确认能拿到宽高、颜色模式、压缩方式和合成像素

交付结果：

- 解码方案确定
- 解压方案确定
- 颜色转换方案确定
- 风险边界写死
- 决定 ICC 读取策略和无 ICC 文件的转换规则

## 阶段 2：接入 PSD 解码模块

当前状态：

- 已完成首版接入。

- 新增 `src/psd.c` / `src/psd.h`
- 实现从 `IStream` 读取到内存
- 实现 PSD header 校验
- 实现 8-bit 各颜色模式分发
- 实现 `Raw / RLE / ZIP / ZIP with prediction`
- 实现到 `BGRA8` 的统一输出
- 输出单帧像素数据

实现建议：

- 按 [PSD 解析设计说明](2026-04-01-psd-parsing-notes.md) 中的最小函数划分实现
- 先打通容器和模式分发，再补齐四种压缩
- 颜色转换单独封装，避免散落在各颜色模式分支里
- 保持失败路径简单明确，不做 fallback 猜测

交付结果：

- `psd_load()` 可以对任意支持的 8-bit PSD 返回一张 `BGRA8` 图

## 阶段 3：接入主加载线程

当前状态：

- 已完成。当前顺序为 `GDI+ -> PSD -> WEBP`。

- 在 `src/viv.c` 中把 `psd_load()` 挂到 `GDI+` 失败分支
- 复用当前第一帧回调和 `HBITMAP` 生成逻辑
- 确保缩略图 / mipmap / preload 与普通图片一致
- 确保失败时走现有错误路径

交付结果：

- 双击 `.psd` 后能正常显示

## 阶段 4：补齐格式可见性

当前状态：

- 已完成首版可见性接入。
- 打开文件对话框、Everything 搜索、命令行帮助、关联设置 UI、本地化与工程文件都已同步。

- 更新打开文件对话框过滤器
- 更新 Everything 搜索白名单
- 更新命令行帮助
- 更新关联设置 UI、资源 ID、本地化文本
- 视需要补 README

交付结果：

- PSD 在产品表层行为上与现有格式一致

## 阶段 5：验证与收尾

当前状态：

- 部分完成。
- `vs2026` 的 `Release|x64` / `Debug|x64` 已完成编译验证。
- 用户已经手动验证“可以打开 PSD”。
- 仍缺 `x86`、更全样本矩阵、失败路径与内存回收的系统化验证。

- 验证 x86 / x64 工程都能编译
- 手动验证全部 8-bit 颜色模式、透明 PSD、超大 PSD、非法 PSD
- 检查切图、切换上一张下一张、拖拽打开、预加载是否正常
- 检查内存回收，确保失败路径不泄漏

交付结果：

- 可以进入合并或进一步扩展阶段

## 8. 主要风险

### 风险 1：PSD 颜色模式过多

PSD 不是一个单一像素格式，而是一组复杂容器。现在目标已经不再是 RGB-only，而是所有 8-bit 颜色模式，因此最大的风险不再是“支不支持”，而是“打开了但颜色不对”。

控制方式：

- 把颜色模式解码和颜色空间转换明确拆成独立模块
- 用覆盖 `Indexed / CMYK / Lab / Duotone / Multichannel` 的样本集验证
- 对每一类颜色模式建立对照输出，避免主观判断“看起来差不多”

### 风险 2：颜色管理成为硬依赖

若目标是“所有 8-bit PSD”，那 `CMYK / Lab / Duotone / Multichannel` 的颜色转换不能靠简单公式糊过去，否则结果会偏色甚至完全错误。

控制方式：

- 第一阶段就确定颜色管理库和 ICC 读取路径
- 不把颜色转换逻辑散写在 PSD 解析代码里
- 用嵌入 profile 和无 profile 两类样本分别验证

### 风险 3：第三方库与现有工程风格不匹配

项目是偏轻量的 C 工程，还包含自带 `libwebp` 与 `minicrt` 配置。若再引入 ZIP 和颜色管理依赖，维护成本会明显上升。

控制方式：

- 优先选纯 C、无额外运行时依赖的库
- 先做 spike，再决定是否正式 vendoring

### 风险 4：超大 PSD 内存压力

PSD 常见于设计稿，尺寸和图层数都可能非常大。即便只读合成图，也可能在加载阶段占用大量内存。

控制方式：

- 在 header 解析阶段先做尺寸和内存预算检查
- 对明显超限文件尽早失败

### 风险 4：用户预期与 Photoshop 显示结果不完全一致

即使只显示合成图，如果 `ICC / Duotone / Multichannel` 转换策略不完整，用户也会认为“PSD 支持有问题”。

控制方式：

- README 和变更说明中明确“支持所有 8-bit PSD 的合成图显示，但不支持图层编辑语义”

## 9. 验收标准

满足以下条件即可认为首版完成：

- 可以直接打开 `.psd`
- 所有 8-bit PSD 颜色模式都能正确显示尺寸和内容
- `Raw / RLE / ZIP / ZIP with prediction` 都能正确解码
- 带透明背景的 PSD 在当前背景色下显示合理
- 切换上一张、下一张、预加载、缩放、最佳适应都正常
- 打开不支持的 PSD 时，不崩溃、不死循环，并能稳定回到失败状态
- UI 中能看到 PSD 已被纳入支持格式与关联选项

## 10. 建议的下一步

如果你要继续推进实现，我建议下一步不要直接大改 `src/viv.c`，而是先做一个很小的技术 spike：

1. 选定 PSD 解析、ZIP 解压和颜色管理方案
2. 先分别验证 `Indexed / CMYK / Lab` 这三类最容易出错的样本
3. 确认颜色转换结果可控后，再接入现有回调链

这样能把风险压到最小，也避免做出一个“RGB 没问题，但其他 8-bit PSD 都偏色”的半成品。
