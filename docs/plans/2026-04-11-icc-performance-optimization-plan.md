# voidImageViewer ICC 性能优化计划

## 1. 结论

当前 ICC 已经把颜色正确性链路打通，但切图性能下降不是偶发问题，而是现有架构下的直接代价。

根因很明确：

1. 加载阶段要把源图按嵌入 profile 或默认 `sRGB` 转成内部 `sRGB BGRA8`。
2. 显示阶段还要再把内部 `sRGB` 转成当前显示器 profile。
3. 转完以后要重新创建 `HBITMAP`，缩小时还可能继续生成 mipmap。
4. 当前每次转换都会重新打开 profile、重新创建 transform，没有复用。

所以本次优化目标不应是“想办法完全消除 ICC 成本”，而应是：

- 避免重复做同一张图、同一显示器、同一 profile 组合下的转换。
- 避免在切图主路径里重复创建昂贵的 ICM 资源。
- 尽量把重活前移到预加载，或者延后到真正需要显示时再做。
- 在不牺牲颜色正确性的前提下，把 ICC 额外成本压到用户能接受的范围。

## 2. 当前瓶颈

### 2.1 每次颜色转换都重新创建 transform

当前实现里，`_color_transform_bgra_internal(...)` 每次都会重新打开源/目标 profile，并创建 `HTRANSFORM`：

```c
source_hprofile = _color_open_profile_from_blob(source_profile);
if (!source_hprofile)
{
    source_hprofile = _color_open_profile_from_path(resolved_source_profile_path);
}

destination_hprofile = _color_open_profile_from_path(resolved_destination_profile_path);
transform = os_CreateMultiProfileTransform(profiles,2,intents,2,0,INDEX_DONT_CARE);

if (os_TranslateBitmapBits(transform,(void *)src_pixels,BM_xRGBQUADS,wide,high,wide * 4,dst_pixels,BM_xRGBQUADS,wide * 4,NULL,0))
```

这意味着只要切到下一张图，就会重复做一次 profile 打开和 transform 创建。对大图来说，真正重的不只是 `TranslateBitmapBits`，还有这些对象的反复初始化和销毁。

### 2.2 切图时至少会发生一次整图 `source -> sRGB`

普通图片和首帧准备都走：

```c
if ((config_icm) && (color_icm_is_active()))
{
    if (!color_transform_to_srgb(bgra_pixels,source_wide,source_high,source_profile,source_profile_path))
```

这一步是“解释源图颜色”必须付出的成本。只要图片有嵌入 ICC，或者默认 `sRGB` 路径没有被快速短路掉，这里就会遍历整张图。

### 2.3 显示前还会发生一次整图 `sRGB -> display`

当前显示缓存重建逻辑：

```c
if (color_transform_srgb_to_display(frame->srgb_pixels,display_pixels,(DWORD)wide,(DWORD)high,current_display_profile_path))
{
    transformed = 1;
}

frame->hbitmap = color_create_hbitmap_from_bgra((DWORD)wide,(DWORD)high,display_pixels);
```

这意味着同一张图如果显示器 profile 不同，就需要重新生成显示缓存。多屏移动窗口时，这条路径一定会被触发。

### 2.4 当前跨屏切换会主动丢弃全部显示缓存

当前逻辑会检查当前显示器 profile 是否变化，如果变了就删掉所有已加载帧的显示缓存：

```c
for(i=0;i<_viv_frame_loaded_count;i++)
{
    if ((!_viv_frames[i].hbitmap) || (string_compare(_viv_frames[i].display_profile_path,current_display_profile_path) != 0))
    {
        _viv_delete_frame_display_cache(&_viv_frames[i]);
        changed = 1;
    }
}
```

这个策略在正确性上没有问题，但性能上比较激进。它把“单显示器变化”处理成了“当前图片集全部显示缓存失效”。

### 2.5 mipmap 进一步放大了重建成本

当前 mipmap 是基于显示位图继续生成的：

```c
*pmip = mem_alloc(sizeof(_viv_mipmap_t));
(*pmip)->hbitmap = CreateCompatibleBitmap(screen_hdc,mip_wide,mip_high);
```

因此 ICC 打开后，显示缓存变慢会直接连带 mipmap 也变慢。缩图浏览、窗口较小时，用户会更容易感知到卡顿。

## 3. 优化目标

### 3.1 主目标

1. 前后切图时，ICC 开启后的体感延迟明显降低。
2. 同一张图在同一显示器上重复显示时，不再重复做同样的显示转换。
3. 多屏切换时，只为真正需要显示的帧重建显示缓存。
4. 预加载路径能尽可能吃掉 ICC 的后台成本。

### 3.2 约束

1. 不能回退为“关 ICC 换性能”。
2. 不能破坏当前 `source -> sRGB -> display` 的正确性模型。
3. 不能把项目推向大规模渲染器重写。
4. 优先小步重构，避免一次性改太多热路径。

## 4. 优化策略

### 4.1 第一优先级：缓存颜色转换资源

这是收益最大、风险相对可控的一步。

建议在 `src/color.c` 内引入轻量级 transform 缓存，至少覆盖：

- 源 profile 标识
- 目标 profile 标识
- 渲染 intent
- 是否是 `destination_is_srgb`

建议缓存内容：

- 已解析的源 `HPROFILE`
- 已解析的目标 `HPROFILE`
- 已创建的 `HTRANSFORM`

设计原则：

1. 以“最近使用的少量组合”为目标，不做复杂大缓存。
2. 首版可以只做固定大小 LRU，例如 `4` 或 `8` 个 transform 槽位。
3. profile path 可直接作为 key；嵌入 ICC blob 需要额外摘要值，例如 CRC32 或快速 hash。
4. 当 key 命中时，直接复用 transform，不再重复 `OpenColorProfile` 和 `CreateMultiProfileTransform`。

预期收益：

- 同一显示器下连续切图时，`sRGB -> display` 的 transform 可长期复用。
- 一批都带 `sRGB IEC61966-2.1` 的图片，`source -> sRGB` 可大量命中同一源 profile。

### 4.2 第二优先级：把显示缓存从“单份”扩展为“按 profile 复用”

当前每帧只有一份 `hbitmap + display_profile_path`。建议改成有限多份显示缓存槽位，而不是只记“最后一次显示在哪个 profile 上”。

推荐方案：

1. 每帧保留 `srgb_pixels` 作为唯一主真值。
2. 每帧增加一个小型 `display cache` 数组，例如 `2` 个槽位。
3. 每个槽位保存：
   - `display_profile_path`
   - `HBITMAP`
   - `mipmap`
   - 最近使用时间戳
4. 命中当前显示器 profile 时直接复用，不需要重建。
5. 跨屏来回切换时，只在首次进入某屏时生成一次，之后来回切换直接命中缓存。

这样可以显著改善双屏用户来回拖窗口的情况，也能避免当前“跨屏后整批帧全部丢缓存”的策略。

### 4.3 第三优先级：把高成本工作更积极地放进预加载路径

当前预加载已经能把下一张图的帧提前准备出来，这条链路本身就是最适合藏 ICC 成本的地方。

优化方向：

1. 预加载阶段不仅生成 `srgb_pixels`，还直接为“当前显示器 profile”构建第一份显示缓存。
2. 如果用户只是在同一显示器上前后切图，那么切换时直接接管已准备好的 `hbitmap`。
3. 当窗口仍在当前显示器且预加载未失效时，前后翻图就不需要在前台线程重新做 `sRGB -> display`。

注意点：

- 预加载启动时要记录“当时的显示器 profile key”。
- 如果预加载期间窗口跨屏，旧的显示缓存可以保留，但激活时只在 key 不匹配时补做当前屏幕版本。

### 4.4 第四优先级：减少不必要的位图与 mipmap 重建

这里有两个方向。

方向 A：延迟创建 mipmap。

当前逻辑里，只要建好显示位图，很多路径很快就继续建 mipmap。可以改成：

1. 首次显示先只确保主 `HBITMAP` 可用。
2. 只有在缩放比例确实需要时才生成 mipmap。
3. mipmap 也要按 `display_profile_path` 挂在对应显示缓存槽位上，而不是跨 profile 复用。

方向 B：避免重复创建等价 `HBITMAP`。

如果当前显示器其实是 `sRGB`，或转换结果可直接走“无需变换”的路径，就不要走“分配新像素 -> 创建新位图 -> 释放临时像素”的完整流程。应尽早短路到复用现有结果。

### 4.5 第五优先级：为“无转换”路径做更强短路

当前已经有一部分短路，但还可以更激进。

目标场景：

1. 源 profile 和目标 profile 实际相同。
2. 图片没有嵌入 ICC，默认就是 `sRGB`。
3. 当前显示器 profile 本身也是标准 `sRGB`。

在这些情况下，应尽量避免：

- 创建 transform
- 分配临时缓冲区
- 像素 copy
- 创建额外显示缓存

理想效果是直接复用 `srgb_pixels` 或由其派生出的单份位图。

## 5. 分阶段实施

### Phase 1：建立性能观测基线

先加调试统计，而不是立刻改逻辑。

建议记录以下时间点：

1. `source -> sRGB` 转换耗时
2. `sRGB -> display` 转换耗时
3. `CreateMultiProfileTransform` 耗时
4. `TranslateBitmapBits` 耗时
5. `HBITMAP` 创建耗时
6. mipmap 生成耗时

建议输出方式：

- `debug_printf`
- 仅 `Debug` 或显式开关下启用
- 输出文件名、分辨率、profile key、是否命中缓存

验收标准：

- 能稳定回答“慢在 source 转换、display 转换、位图创建、还是 mipmap”。

### Phase 2：实现 transform 缓存

这是第一批真正代码改动。

实施内容：

1. 在 `src/color.c` 增加 transform cache。
2. 为嵌入 ICC blob 生成稳定 key。
3. 重写 `_color_transform_bgra_internal(...)`，先查缓存，未命中才创建 transform。
4. 处理 cache 生命周期与清理。

验收标准：

- 同一显示器连续切图时，`CreateMultiProfileTransform` 次数显著下降。
- 功能结果与现有 ICC 输出保持一致。

### Phase 3：引入每帧多槽位显示缓存

实施内容：

1. 扩展 `_viv_frame_t`，从“单显示缓存”升级为“少量按 profile 的显示缓存槽位”。
2. `_viv_ensure_frame_display_cache(...)` 改成“查槽位命中”，而不是只看单一 `display_profile_path`。
3. `_viv_update_display_profile_if_needed(...)` 改成“懒失效 + 按需构建”，不要先清空全部。

验收标准：

- 双屏来回切换同一张图时，第二次进入同一屏不再重建显示缓存。
- 单屏正常浏览行为不退化。

### Phase 4：把 ICC 成本前移到预加载

实施内容：

1. 预加载完成后，直接为当前显示器生成显示缓存。
2. 激活预加载图像时优先接管现成缓存。
3. 如果预加载和激活时的显示器不同，只补生成缺失 profile 的版本。

验收标准：

- 开启预加载后，前后切图的前台阻塞进一步下降。
- 预加载路径不引入新的所有权和释放问题。

### Phase 5：延迟 mipmap 与进一步短路

实施内容：

1. 把 mipmap 改成首次需要时再创建。
2. 完善“源/目标 profile 相同”的零转换路径。
3. 如果当前显示器为标准 `sRGB`，尽量走最短路径。

验收标准：

- 小图、窗口缩放、缩略显示场景下切换更顺。
- ICC 正确性测试结果不变。

## 6. 风险与约束

### 6.1 最大风险不是性能，而是缓存一致性

ICC 这块最容易出错的不是“算法不快”，而是：

- transform key 算错，导致命中错误缓存
- display cache 生命周期处理不完整，导致悬空 `HBITMAP` 或二次释放
- 预加载和前台激活之间所有权转移再次出错

所以每一步都必须优先保证：

1. key 正确
2. 生命周期清晰
3. 失效条件可解释

### 6.2 不建议一步到位做后台并行颜色转换

虽然把 `sRGB -> display` 完全异步化理论上也能改善前台卡顿，但当前代码对帧对象所有权、预加载切换和 GDI 资源生命周期已经比较敏感。

在没有先完成 transform 缓存和显示缓存分层前，不建议先上新的线程复杂度。否则很容易把“慢”换成“偶发崩溃”或“颜色缓存错乱”。

## 7. 验证方案

### 7.1 正确性回归

每次改动 ICC 性能逻辑后，至少回归：

- `C:\Users\LiuYang\Desktop\sRGB_Gray.png`
- `C:\Users\LiuYang\Desktop\sRGB_Gray.jpg`
- `C:\Users\LiuYang\Desktop\fac.psd`
- `D:\SF_ArtVault\RefPictures\设计参考\武器\M4A1\500px-20131014_171117_.jpg`

预期：

- `sRGB_Gray` 系列仍然是灰底灰字，不回退成红底红字。
- `PSD/JPG` 样本不崩溃。

### 7.2 性能回归

建议补一组固定观察动作：

1. 在单屏上连续按左右键切换 20 次。
2. 开启 `preload next` 再重复一次。
3. 把窗口拖到第二块屏幕后再重复一次。
4. 双屏之间来回拖动同一张 ICC 图片数次。

建议记录：

- 是否出现明显可见卡顿
- `Debug` 日志中的转换耗时
- transform cache 命中率
- display cache 命中率

## 8. 推荐实施顺序

建议按下面顺序推进，不建议换序：

1. 先做观测埋点，确认重灾区。
2. 再做 transform 缓存，这是收益最高的一步。
3. 然后做每帧多槽位显示缓存，解决跨屏和重复切图。
4. 再把显示缓存前移到预加载。
5. 最后做 mipmap 延迟和更强短路。

这样做的原因是：

- 前两步主要解决“重复创建昂贵 ICM 资源”。
- 中间两步解决“重复做已经做过的显示转换”。
- 最后一步再抠“显示链末端的附加成本”。

## 9. 成功标准

完成本计划后，至少应满足：

1. ICC 开启时，前后切图不再出现明显线性变慢。
2. 同一显示器上浏览一批带 ICC 的图片时，性能下降控制在可接受范围。
3. 双屏来回切换时，第二次进入同一屏能明显复用缓存。
4. 所有已有 ICC 正确性测试样本继续通过。
5. 不引入新的崩溃、悬空指针或缓存污染问题。

## 10. 本次建议

如果只选一项马上做，我建议先做“transform 缓存 + 调试计时”。

原因很直接：

- 它最接近当前根因。
- 它对数据结构冲击最小。
- 它能立刻告诉我们后面的收益上限。
- 它为后续显示缓存优化提供数据依据。
