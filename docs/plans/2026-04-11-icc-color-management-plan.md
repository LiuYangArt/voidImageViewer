# voidImageViewer ICC 色彩管理实施计划

## 1. 结论

可以做，而且应该按完整链路来做，不应继续停留在“读图时尽量尊重嵌入 ICC”的半实现状态。

本次计划的目标定义如下：

- 覆盖当前已支持的全部图片格式，而不是只修常规 `GDI+` 分支。
- 当图片包含嵌入 ICC 时，按该 ICC 解释源图颜色。
- 当格式不支持嵌入 ICC，或图片本身没有 ICC 时，默认按 `sRGB` 处理。
- 当用户使用多显示器时，软件必须识别当前窗口所在显示器，并切换到对应显示器的 ICC profile。
- 继续保持项目现有的轻量原生架构，不引入 Direct2D、WIC 全量重写或跨平台抽象层。

推荐实施方向不是改造整条渲染器，而是新增一层统一颜色管理：

1. 加载阶段把所有格式统一归一化为 `sRGB BGRA8` 主缓存。
2. 显示阶段再根据当前窗口所在显示器 profile，生成显示缓存和 mipmap。
3. 最终仍尽量复用现有 `HBITMAP + BitBlt/StretchBlt` 显示路径。

这样做的好处是：

- 可以兼容现有所有格式分支。
- 可以支持多屏切换时重建显示缓存。
- 可以把源图颜色解释和显示器颜色匹配拆成两个明确阶段，避免依赖 `GDI+` 或 `GDI` 的隐式行为。
- 改动集中，风险可控，符合本项目“小步、定点、可维护”的方向。

## 2. 当前代码事实

### 2.1 已有但不完整的颜色管理入口

仓库里已经有 `config_icm`：

- `src/config.c`
- `src/config.h`

并且常规格式加载时会走：

```c
if (config_icm)
{
    load_ret = os_GdipLoadImageFromStreamICM(stream,&image);
}
else
{
    load_ret = os_GdipLoadImageFromStream(stream,&image);
}
```

对应文件：`src/viv.c`

这说明项目已经尝试启用图片侧的嵌入色彩信息，但这还不是完整色彩管理，因为：

- 这条路径只覆盖常规 `GDI+` 可解码格式。
- 后续显示仍是 `HBITMAP`、`BitBlt`、`StretchBlt` 为主的普通 GDI 渲染链。
- 没有看到“当前显示器 ICC profile”参与决策的代码。
- 没有多屏切换时重建颜色缓存的机制。

### 2.2 专用格式路径目前不具备完整 ICC

`PSD`：

```c
if (resource_id == PSD_RESOURCE_ID_ICC_PROFILE)
{
    /* Parsed only so the section walk stays correct for future color-management work. */
}
```

对应文件：`src/psd.c`

这意味着 PSD 的 ICC 目前只是“识别到了资源块”，并没有真正接入颜色转换。

`WEBP`：

当前专用解码路径把像素写到 `HBITMAP`：

```c
SetDIBits(viv_webp->mem_hdc,hbitmap,0,viv_webp->high,pixels,&bmi,DIB_RGB_COLORS);
```

对应文件：`src/viv.c`

但仓库里还没有看到对 WEBP `ICCP` chunk 的解析与应用代码，因此 WEBP 也不能算完整 ICC 支持。

### 2.3 多显示器定位已有基础设施

仓库里已经有按窗口和鼠标位置获取显示器区域的封装：

- `os_MonitorRectFromWindow()`
- `os_MonitorRectFromRect()`
- `os_MonitorRectFromCursor()`

对应文件：

- `src/os.c`
- `src/os.h`

这很重要，因为“当前窗口属于哪个显示器”并不需要从零写起。缺的不是显示器判定，而是“根据该显示器拿到 ICC profile 并驱动显示缓存重建”的后半段。

### 2.4 当前没有正式用户可见开关

虽然已有 `config_icm`，但目前没有对应的：

- 资源控件
- 本地化文案
- 选项页读写绑定

对应文件现状：

- `src/config.c` / `src/config.h` 有配置
- `src/viv.c` 选项页未接线
- `res/resource.h` / `res/voidImageViewer.rc` 没有 ICM 相关控件
- `src/localization*.h` 没有对应文案

因此计划里必须明确：首版完整 ICC 是否作为正式用户选项暴露。我的建议是：暴露，而且默认开启。

## 3. 目标与范围

### 3.1 必须达成的目标

1. 所有当前支持的格式都纳入统一色彩管理策略。
2. 嵌入 ICC 优先。
3. 无嵌入 ICC 时默认按 `sRGB`。
4. 当前窗口移动到不同显示器时，自动切换到该显示器 ICC。
5. 对静态图和动画帧都适用。
6. 不引入新的外部渲染框架。

### 3.2 当前支持格式范围

计划必须覆盖：

- `BMP`
- `GIF`
- `ICO`
- `JPEG/JPG`
- `PNG`
- `TIF/TIFF`
- `PSD`
- `WEBP`

### 3.3 本阶段不做的内容

本计划不把下面这些内容作为首版目标：

- HDR / 10-bit / scRGB / ICC v4 以外的高级显示管线专题优化
- 打印色彩管理
- 手动选择显示器 profile 的复杂 UI
- 图像编辑级别的软打样 / 渲染意图高级控制面板
- 全量更换为 Direct2D/WIC 渲染器

## 4. 核心设计决策

### 4.1 统一内部工作空间：`sRGB BGRA8`

推荐把 `sRGB BGRA8` 作为所有格式进入显示系统前的统一主缓存格式。

理由：

- 当前项目已经大量依赖 8-bit 位图、`HBITMAP`、mipmap 和 GDI 缩放链。
- `sRGB` 是“无 ICC 默认值”最合理的默认空间，符合用户要求。
- 统一主缓存后，多屏切换只需要处理“sRGB -> 当前显示器 profile”的二次变换，不必对每种源格式单独处理。
- 能最大化复用现有渲染路径，避免大规模重写 `src/viv.c`。

因此，推荐流程是：

- 源图像像素 + 源 profile
- 先转换为 `sRGB BGRA8` 主缓存
- 再根据当前显示器 profile 生成显示缓存
- 现有绘制逻辑继续消费显示缓存

### 4.2 源图 profile 选择规则

必须统一定义所有格式的源 profile 规则：

1. 如果文件有有效嵌入 ICC，使用嵌入 ICC。
2. 如果格式支持 gamma/chromaticity 但没有完整 ICC，优先评估是否能稳定转为等效颜色空间；若实现成本过高，首版统一回退为 `sRGB`，但需要在计划期明确记录。
3. 如果格式不支持嵌入 ICC，或文件里没有 ICC，默认按 `sRGB`。
4. 不允许因为“没有 ICC”而走未定义行为或设备相关色空间。

### 4.3 显示器 profile 选择规则

推荐规则：始终以当前主窗口最近的显示器为准。

具体策略：

- 使用 `MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)` 识别当前显示器。
- 通过 `GetMonitorInfo` 的扩展版本拿到显示器设备名。
- 基于该设备名创建显示器 DC。
- 用 `GetICMProfileW` 读取当前输出 profile 路径。
- 将 profile 路径作为显示缓存的 key 之一。

当下面任一事件发生时，都要重新检查当前显示器 profile：

- 启动时
- 打开新图时
- `WM_MOVE`
- `WM_SIZE`
- 全屏进入 / 退出
- `WM_DISPLAYCHANGE`
- 未来如有需要，可补 `WM_WINDOWPOSCHANGED` 或 DPI / 设备变化相关消息

### 4.4 不依赖隐式 GDI/GDI+ 作为最终方案

当前 `GdipLoadImageFromStreamICM` 可以作为事实依据和过渡参考，但不应成为首版完整 ICC 的唯一依赖。

原因：

- 它只覆盖常规格式，不覆盖 `PSD/WEBP` 的专用解码路径。
- 仓库当前最终渲染链还是 `HBITMAP + BitBlt/StretchBlt`，这会让“显示器 profile 何时生效”变得不透明。
- 当用户跨显示器移动窗口时，不能明确控制缓存何时需要重建。

因此，首版可接受的方案必须满足：

- 源图 profile 的选择是显式的。
- 显示器 profile 的选择是显式的。
- 颜色转换的触发时机是显式的。
- 缓存失效逻辑是显式的。

## 5. 推荐架构

### 5.1 新增统一颜色管理模块

建议新增单独模块，例如：

- `src/color.c`
- `src/color.h`

职责：

- 解析和管理源 profile
- 获取当前显示器 profile
- 创建颜色转换对象
- 执行像素转换
- 维护缓存键与失效逻辑

不要把这些逻辑继续塞进 `src/viv.c` 零散分支里。

### 5.2 新的数据分层

建议把现有“只存 `HBITMAP`”的帧模型扩展成双缓存模型：

1. 主缓存：`sRGB BGRA8`
2. 显示缓存：当前显示器 profile 对应的 `HBITMAP` / mipmap

可以用类似下面的思路，不要求名字完全一致：

```c
typedef struct viv_color_frame_s
{
    BYTE *srgb_pixels;
    DWORD width;
    DWORD height;
    int has_alpha;
    int delay;

    HBITMAP display_hbitmap;
    _viv_mipmap_t *display_mipmap;
    wchar_t display_profile_path[STRING_SIZE];
} viv_color_frame_t;
```

这样设计后：

- 加载新图时只需先建立 `srgb_pixels`。
- 当前显示器 profile 变化时，只重建 `display_hbitmap` 和 `display_mipmap`。
- 动画帧也能统一处理。

### 5.3 Windows 颜色转换实现建议

建议优先使用系统自带颜色管理 API，而不是引入额外第三方库。

本机 SDK 已确认存在：

- `OpenColorProfileW`
- `CreateMultiProfileTransform`
- `TranslateBitmapBits`
- `GetICMProfileW`

对应头文件：

- `Icm.h`
- `wingdi.h`

推荐原因：

- 项目本来就是 Windows-only。
- 避免增加第三方依赖和工程同步负担。
- 更符合当前仓库的动态加载系统 API 风格。

实现建议：

1. 把源 ICC blob 或默认 `sRGB` profile 打开为 `HPROFILE`。
2. 把显示器 profile 路径打开为目标 `HPROFILE`。
3. 用 `CreateMultiProfileTransform` 创建 transform。
4. 用 `TranslateBitmapBits` 把 `BGRA8` 主缓存转换为目标显示缓存。
5. Alpha 通道保留原值，只转换 RGB。

如果系统 API 在某些格式或 profile 上表现不稳定，再评估引入 `Little CMS 2` 作为第二选择；但这不应作为首选路线。

### 5.4 显示器 profile 获取建议

建议在 `src/os.c` / `src/os.h` 新增一个封装，例如：

```c
int os_GetMonitorColorProfileFromWindow(HWND hwnd, wchar_t *profile_path, DWORD profile_path_capacity);
```

内部流程建议：

1. `MonitorFromWindow`
2. `GetMonitorInfo` 扩展版本拿到设备名
3. `CreateDCW(L"DISPLAY", device_name, NULL, NULL)`
4. `GetICMProfileW`
5. 清理 DC

同时建议再新增一个轻量比较函数，用于判断 profile 是否真的变化，避免每次移动窗口都无意义重建。

## 6. 各格式接入策略

### 6.1 常规 `GDI+` 格式

覆盖：

- `BMP`
- `GIF`
- `ICO`
- `JPEG/JPG`
- `PNG`
- `TIF/TIFF`

计划要求：

- 不再把 `GDI+` 的 ICM 调用视为最终结果。
- 需要能把解码结果交给统一颜色管理层，生成 `sRGB` 主缓存。

推荐步骤：

1. 先保留当前 `GDI+` 加载路径用于解码和动画帧选择。
2. 补做一个验证 spike，确认在“禁用隐式 ICM”时是否能稳定拿到原始解码像素。
3. 若能稳定获取，则统一改为：
   - 解码得到像素
   - 解析源 profile
   - 转换到 `sRGB BGRA8`
4. 若 `GDI+` 对某些格式无法稳定给出所需 profile 信息，则把“提取 ICC blob”的缺口列为子任务，不允许直接跳过。

原则：首版可以接受阶段性过渡实现，但最终交付不能只依赖 `GdipLoadImageFromStreamICM` 这一个黑盒行为。

### 6.2 `WEBP`

计划要求：

- 解析 `ICCP` chunk
- 若存在 ICC，作为源 profile
- 若不存在，默认 `sRGB`
- 解码后的像素统一进入颜色管理层

当前 `WEBP` 已经有独立解码模块，这是好事，因为接入点清晰。

建议：

- 把 ICC 提取放进 `src/webp.c` 或新的 `src/color_webp.c`
- 不要继续在 `_viv_webp_frame_proc()` 里直接把像素视为最终显示像素
- 先生成 `sRGB` 主缓存，再生成显示缓存

### 6.3 `PSD`

计划要求：

- 正式使用 `PSD_RESOURCE_ID_ICC_PROFILE` 资源块
- 若存在 ICC，作为源 profile
- 若不存在，默认 `sRGB`
- 色彩转换在 PSD 扁平合成图归一化完成后执行

`src/psd.c` 当前已经把 ICC 资源块位置识别出来，因此这里不需要重新设计格式入口，只要把“预留注释”变成正式接入即可。

### 6.4 动画帧

覆盖：

- `GIF`
- `WEBP` 动画
- 未来可能的其他多帧格式

建议策略：

- 加载线程输出每帧的 `sRGB BGRA8`。
- 首帧优先建立显示缓存，保证首帧打开速度。
- 其他帧可按现有 preload 思路继续预建，也可以采用延迟生成显示缓存。
- 当显示器 profile 改变时：
  - 当前帧立即重建
  - 其余帧可异步 / 懒重建，避免窗口跨屏瞬间卡死

## 7. UI、配置与本地化计划

### 7.1 配置策略

保留现有 INI 键：

- `icm=1`

首版建议：

- 默认开启
- 作为正式用户选项显示
- 若关闭，则整条颜色管理链停用，行为回到未管理显示

### 7.2 选项页

建议把选项放进当前 View 页，而不是额外新建页面。

原因：

- 当前 View 页已经承载缩放、背景色、缓存、自动缩放等显示行为。
- ICC 属于显示语义，不是文件关联或控制逻辑。
- 增一个勾选框改动最小。

建议文案：

- 中文：`启用色彩管理 (ICC)`
- 英文：`Enable Color Management (ICC)`

需要联动修改：

- `res/resource.h`
- `res/voidImageViewer.rc`
- `src/localization.h`
- `src/localization_en_us.h`
- `src/localization_zh_cn.h`
- `src/viv.c`

## 8. 分阶段实施计划

### Phase 1: 颜色管理基础设施

目标：先把“显示器 profile 获取”和“统一颜色转换模块”搭起来。

任务：

- 新增 `src/color.c` / `src/color.h`
- 在 `src/os.c` / `src/os.h` 新增“按窗口获取显示器 profile 路径”封装
- 动态加载或静态接线 `OpenColorProfileW` / `CreateMultiProfileTransform` / `TranslateBitmapBits`
- 实现 `sRGB` 默认 profile 的创建 / 获取逻辑
- 定义统一帧缓存结构

完成标准：

- 给定一块 `BGRA8` 像素和一个目标显示器 profile，能稳定输出转换后的像素
- 当前窗口能拿到最近显示器的 profile 路径

### Phase 2: 主缓存与显示缓存分层

目标：把现有“只有 `HBITMAP`”的帧模型改成“双缓存模型”。

任务：

- 扩展帧结构，增加 `sRGB BGRA8` 主缓存
- 增加显示缓存重建函数
- 增加显示 profile 缓存键比较逻辑
- 修改释放逻辑，确保像素缓存、`HBITMAP` 和 mipmap 一起正确释放

完成标准：

- 打开一张普通图片后，能同时存在主缓存和显示缓存
- 不泄漏内存 / GDI 对象

### Phase 3: 常规 `GDI+` 格式接入

目标：让 `BMP/GIF/ICO/JPEG/PNG/TIFF` 都经过统一颜色管理层。

任务：

- 对当前 `GDI+` 解码流程做验证性 spike
- 明确源 profile 获取手段
- 输出 `sRGB BGRA8` 主缓存
- 保持现有动画帧逻辑可用

完成标准：

- 常规格式在启用 ICC 时进入统一主缓存流程
- 无 ICC 的样本按 `sRGB` 正常显示

### Phase 4: `WEBP` / `PSD` 接入

目标：把专用解码路径纳入同一条色彩链。

任务：

- `WEBP`：解析 `ICCP` chunk，接入统一颜色层
- `PSD`：正式使用 resource 1039 ICC 数据，接入统一颜色层
- 保证这两类格式在无 ICC 时默认 `sRGB`

完成标准：

- `WEBP` / `PSD` 与常规格式在颜色策略上保持一致
- 不再存在“普通格式支持 ICC，专用格式不支持”的分裂行为

### Phase 5: 多显示器切换与缓存失效

目标：让窗口跨屏时颜色正确刷新。

任务：

- 在 `WM_MOVE` / `WM_SIZE` / `WM_DISPLAYCHANGE` / 全屏切换路径中增加 profile 变化检查
- 若显示器 profile 变化，则重建当前图像显示缓存
- 对动画帧采取“当前帧立即重建、其他帧按需重建”的策略

完成标准：

- 窗口拖到另一台显示器时，颜色会切换到新显示器 profile
- 不出现缓存错用、闪烁异常或崩溃

### Phase 6: UI、配置、本地化与文档

目标：把功能完整暴露给用户。

任务：

- 把 `config_icm` 接入 View 页复选框
- 更新中英文文案
- 确认 INI 读写行为与 UI 一致
- 更新 README / Changes / 后续文档（如需要）

完成标准：

- 用户能在选项页明确开关 ICC
- 配置持久化正确

### Phase 7: 样本验证与回归

目标：确认行为正确，而不是“编译通过就算完成”。

任务：

- 建立样本矩阵
- 对单屏和双屏场景分别验证
- 对静态图、动画图、无 ICC 图、嵌入 ICC 图分别验证
- 检查大图、缩放、缓存、预加载和跨屏切换性能

完成标准：

- 手动验证矩阵通过
- 没有明显偏色、闪烁、卡顿或资源泄漏

## 9. 文件改动清单

预计至少会涉及：

- `src/viv.c`
- `src/os.c`
- `src/os.h`
- `src/webp.c`
- `src/psd.c`
- `src/config.c`
- `src/config.h`
- `res/resource.h`
- `res/voidImageViewer.rc`
- `src/localization.h`
- `src/localization_en_us.h`
- `src/localization_zh_cn.h`
- `vs2019/voidImageViewer.vcxproj`（如新增文件）
- `vs2026/voidImageViewer.vcxproj`（如新增文件）

以及新增：

- `src/color.c`
- `src/color.h`

## 10. 风险与规避

### 风险 1：常规格式的源 profile 获取不稳定

问题：`GDI+` 对不同格式和 metadata 的暴露能力不完全一致。

规避：

- 先做 spike，确认能否稳定取得嵌入 profile 或等效颜色信息。
- 若不稳定，把“提取 profile blob”拆成单独子任务，不把黑盒行为直接当最终实现。

### 风险 2：大图和动画性能下降

问题：双阶段缓存和跨屏重建会增加 CPU 和内存开销。

规避：

- 主缓存统一为 `BGRA8`，避免重复格式转换。
- 显示缓存只在 profile 变化时重建。
- 动画帧采用懒重建策略。

### 风险 3：Alpha 与颜色转换顺序错误

问题：如果对预乘或背景混合后的颜色再做 profile 转换，可能引入边缘发灰或色边。

规避：

- 先明确主缓存像素约定。
- 颜色转换只作用于 RGB，Alpha 保持原值。
- 在透明图样本上做专项验证。

### 风险 4：窗口跨屏时判断不一致

问题：窗口横跨两块显示器时，“该用哪块屏的 profile”必须明确。

规避：

- 首版统一使用 `MonitorFromWindow(..., MONITOR_DEFAULTTONEAREST)`。
- 文档明确这是“以窗口最近显示器为准”的定义，而不是平均混合。

### 风险 5：显示器 profile 缺失或无效

问题：某些用户系统没有有效 profile，或 profile 文件损坏。

规避：

- 获取失败时回退到 `sRGB` 目标 profile。
- 不允许因为 profile 异常导致图片无法打开。

## 11. 验证计划

### 11.1 样本矩阵

至少准备：

- 无 ICC 的 `JPEG/PNG/WEBP/PSD/GIF`
- 嵌入 `sRGB` ICC 的样本
- 嵌入 `Adobe RGB` ICC 的样本
- 嵌入 `Display P3` ICC 的样本
- `PSD` 含 ICC 的样本
- `WEBP` 含 `ICCP` chunk 的样本
- 动画 `GIF`
- 动画 `WEBP`

### 11.2 功能场景

必须覆盖：

- 打开图片首次显示
- 上一张 / 下一张切换
- 预加载开启时的切换
- 缩放、窗口大小变化
- 全屏切换
- 双显示器之间拖动窗口
- 一块屏关闭 ICC、另一块屏启用 ICC 的差异场景

### 11.3 结果判断

合格标准：

- 无 ICC 图片默认按 `sRGB` 显示
- 含 ICC 图片与参考查看器观感一致
- 跨屏拖动时颜色会切换，不是保持原屏结果
- 所有当前支持格式都遵守同一套规则
- 没有新增崩溃、黑图、明显卡顿或 GDI 泄漏

## 12. 验收标准

当下面这些条件全部满足，才算 ICC 支持任务完成：

1. 当前支持的所有格式都纳入统一颜色管理链。
2. 无 ICC 默认 `sRGB` 已落地且验证通过。
3. 当前窗口所在显示器 profile 能被识别并参与显示。
4. 窗口跨屏移动时会重建显示缓存。
5. `config_icm` 已成为正式 UI 开关，默认开启。
6. 单屏、双屏、静态图、动画图、无 ICC 图、嵌入 ICC 图都完成手动验证。

## 13. 推荐实施顺序

推荐按下面顺序执行，不要一上来同时改所有格式：

1. 先完成颜色管理模块与显示器 profile 获取。
2. 再完成主缓存 / 显示缓存分层。
3. 再接入常规 `GDI+` 格式。
4. 然后接入 `WEBP` / `PSD`。
5. 最后补 UI、本地化和完整验证。

这个顺序的好处是：

- 先把基础设施做稳，再接格式分支。
- 能尽早验证多显示器 profile 识别是否正确。
- 不会把格式支持和显示器切换问题混成一个难以排查的大改动。
