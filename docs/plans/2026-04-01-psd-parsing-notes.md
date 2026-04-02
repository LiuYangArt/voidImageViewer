# voidImageViewer PSD 解析设计说明

## 1. 目的

这份文档是 [PSD 支持计划](2026-04-01-psd-support-plan.md) 的技术补充，目标不是完整讲清 Photoshop 全格式兼容，而是明确：

- 首版 PSD 看图支持到底做什么
- 实现时应该参考哪些开源代码
- PSD 文件里哪些结构必须解析
- `voidImageViewer` 应该怎样把 PSD 接进现有加载链路

建议在真正开始编码前，先读完本文件，再回到主计划拆具体任务。

## 2. 适合参考的开源实现

## `stb_image.h`

定位：

- 最适合当前项目首版目标的参考代码
- 重点学习“如何读取 PSD 的最终合成图”

为什么值得看：

- 代码短，依赖极少
- 思路非常接近轻量看图器，而不是编辑器
- 对当前项目最有用的是它的 PSD header 解析、RLE 解码和通道重组思路

阅读重点：

- PSD 头部字段校验
- 8-bit RGB/RGBA 输出路径
- `Raw` / `RLE` 两种压缩处理
- 平面通道数据重组为交错像素数据

适合借鉴，不建议原样硬搬整个单头库。

## ImageMagick `coders/psd.c`

定位：

- 生产级 PSD/PSB 支持参考

为什么值得看：

- 异常情况和边界处理更完整
- 适合补充 `stb_image` 没覆盖到的错误分支

阅读重点：

- 长度字段与越界检查
- 非法文件的失败路径
- 各颜色模式和位深的拒绝策略

不适合直接并入当前项目，因为工程太重，依赖和风格都不匹配。

## GIMP PSD 插件

定位：

- 如果未来考虑图层、蒙版、混合模式，这是很好的长期参考

对当前项目的价值：

- 用来理解哪些 Photoshop 语义会让实现复杂度急剧上升
- 用来证明首版不应碰完整图层重建

当前阶段只需要把它当上界参考，不建议按它的复杂度开工。

## `psd-tools`

定位：

- 最适合作为测试对照工具

用途：

- 用同一个 PSD 导出 PNG
- 对比你解出来的尺寸、透明度和颜色是否一致

它是 Python 项目，不适合作为 `voidImageViewer` 的运行时依赖，但非常适合用来做样本验证。

## `Little CMS 2`

定位：

- 颜色管理参考实现和可选依赖

为什么值得看：

- 如果目标是“所有 8-bit PSD”，那 `CMYK / Lab / Duotone / Multichannel` 的显示正确性离不开颜色空间转换
- 它适合承担“把 PSD 源颜色空间转换到 sRGB”的职责

对当前项目的价值：

- 它不是 PSD 解析器，但很可能是支持所有 8-bit PSD 所必需的辅助库

## `miniz` / `zlib`

定位：

- ZIP 与 ZIP prediction 压缩支持的参考实现

为什么值得看：

- 如果目标是“所有 8-bit PSD”，就不能只支持 `Raw / RLE`
- PSD 的 `ZIP / ZIP with prediction` 需要可靠的解压支持

## 3. PSD 首版实现边界

按新的目标，首版建议支持下面这组子集：

- 扩展名：`.psd`
- 版本：标准 PSD，不含 `.psb`
- 位深：`8-bit`
- 颜色模式：`Grayscale / Indexed / RGB / CMYK / Multichannel / Duotone / Lab`
- 压缩：`Raw / RLE / ZIP / ZIP with prediction`
- 输出：最终合成图

首版明确不做：

- `PSB`
- `Bitmap` 1-bit
- `16-bit` / `32-bit`
- 图层逐层重建
- 文字层、智能对象、调整图层等 Photoshop 语义

如果不把边界收紧，项目很快会从“轻量看图器扩展一个格式”变成“做半个 PSD 渲染器”。

## 4. PSD 文件结构最小必读

PSD 采用大端序存储，解析时所有 `WORD` 和 `DWORD` 都必须按 big-endian 读取。

文件整体顺序如下：

1. File Header Section
2. Color Mode Data Section
3. Image Resources Section
4. Layer and Mask Information Section
5. Image Data Section

对当前项目首版最重要的是：

- File Header Section
- Color Mode Data Section
- Image Resources Section
- Image Data Section

现在不能再把其余区块全部无脑跳过，因为：

- `Indexed Color` 需要调色板
- `Duotone` 需要 color mode data
- `CMYK / Lab / Duotone / Multichannel` 往往需要结合 ICC profile 或颜色资源做转换

## 5. File Header Section

Header 固定长度 26 字节。

字段如下：

```text
4 bytes  Signature      = '8BPS'
2 bytes  Version        = 1
6 bytes  Reserved       = 0
2 bytes  Channels       = 1..56
4 bytes  Height
4 bytes  Width
2 bytes  Depth          = 1, 8, 16, 32
2 bytes  Color Mode
```

对当前项目的首版校验建议：

- `Signature` 必须是 `8BPS`
- `Version` 必须是 `1`
- `Width > 0`
- `Height > 0`
- `Depth == 8`
- `Color Mode` 属于支持集合
- `Channels` 满足对应颜色模式的合法范围

如果任何一项不满足，直接返回“不支持的 PSD”，不要继续猜。

## 6. 首版真正要处理的区块

## Color Mode Data Section

结构：

```text
4 bytes length
N bytes data
```

首版处理：

- 读取长度
- 跳过数据

现在这里必须按颜色模式决定是否解析：

- `Indexed Color`：读取调色板
- `Duotone`：读取并保存 Duotone 数据，供后续颜色转换使用
- 其他颜色模式：可跳过

## Image Resources Section

结构：

```text
4 bytes length
N bytes data
```

首版处理：

- 读取长度
- 定位并提取 ICC profile 等和显示颜色相关的资源

注意：

- 如果目标是“所有 8-bit PSD”，ICC profile 已经不再是可选增强，而是颜色正确性的关键输入之一

## Layer and Mask Information Section

结构：

```text
4 bytes length
N bytes data
```

首版处理：

- 读取长度
- 整段跳过

原因：

- 当前目标是使用最终合成图
- 不做图层单独渲染，就没必要解析这里的复杂结构

## Image Data Section

这是首版的核心。

结构：

```text
2 bytes compression
remaining bytes payload
```

压缩值：

- `0 = Raw`
- `1 = RLE`
- `2 = ZIP`
- `3 = ZIP with prediction`

现在必须支持 `0 / 1 / 2 / 3` 全部四种压缩。

## 7. 四种压缩的实现要求

## Raw

对于 8-bit PSD，Raw 数据的排列方式是按通道平面存储：

```text
all Red samples
all Green samples
all Blue samples
optional Alpha samples
```

也就是不是：

```text
RGBARGBARGBA
```

而是：

```text
RRR... GGG... BBB... AAA...
```

实现时要为每个通道读出 `width * height` 字节，再按颜色模式重组为显示所需的中间表示。

## RLE

PSD 的 RLE 不是一个整体流，而是按“每个通道的每一行”分别编码。

数据结构是：

1. 先有一张行长度表
2. 然后按顺序跟着每个通道、每一行的 PackBits 数据

对标准 PSD：

- 行长度表项大小是 `2 bytes`
- 表项数量是 `channels * height`

解码规则遵循 PackBits：

- `0..127`：拷贝接下来 `n + 1` 个原始字节
- `129..255`：重复下一个字节 `257 - n` 次
- `128`：noop

建议实现：

- 先为每个通道申请 `width * height` 缓冲
- 再逐行解 RLE，写到通道平面缓存中
- 每一行必须严格解到 `width` 个字节，过多或过少都判失败

## ZIP 与 ZIP with prediction

如果目标是所有 8-bit PSD，就不能跳过 ZIP 系列压缩。

建议处理方式：

- `ZIP`：直接解压到通道平面缓冲
- `ZIP with prediction`：先解压，再按颜色模式执行 predictor 还原

这里不建议手写 Deflate，优先引入成熟开源实现。

## 8. 颜色与像素重组

`voidImageViewer` 当前显示链路最终要落到 `HBITMAP`，因此 PSD 解码层最好输出一份连续的 `32-bit BGRA` 或 `RGBA` 像素缓冲，再交给现有位图创建逻辑。

建议内部统一输出：

- `BGRA8`

映射规则：

- `dst[0] = B`
- `dst[1] = G`
- `dst[2] = R`
- `dst[3] = A`

但在输出 `BGRA8` 之前，应先按颜色模式完成归一化：

### Grayscale

- 1 个灰度通道扩展为 `R = G = B = gray`
- 若存在额外 alpha，则写入 `A`

### Indexed

- 从 Color Mode Data 读取 256 项调色板
- 先把像素索引映射为 RGB
- 再写入 `BGRA8`

### RGB

- `R/G/B` 直接重组
- 若存在第 4 通道，视为 alpha

### CMYK

- 先得到 `C/M/Y/K`
- 若有 embedded ICC，优先通过 ICC 转到 sRGB
- 若无 ICC，也必须定义固定转换策略，不能临时猜测

### Lab

- 先得到 `L/a/b`
- 再通过标准颜色管理流程转到 sRGB

### Duotone

- 必须结合 Color Mode Data 和颜色管理处理
- 不建议手写近似公式后直接宣称“支持”

### Multichannel

- 必须先明确通道语义和颜色转换路径
- 这是所有 8-bit 模式里风险最高的一类，建议优先用参考实现或颜色管理库校验

只有在这些模式都被归一化到显示色彩空间后，才应该统一输出 `BGRA8`。

## 9. 建议的内部结构

可以新增一个极小的内部上下文：

```c
typedef struct _viv_psd_s
{
    DWORD width;
    DWORD height;
    WORD channels;
    WORD depth;
    WORD color_mode;
    WORD compression;
} _viv_psd_t;
```

以及一些小型辅助函数：

```c
static int psd_read_u16_be(IStream *stream, WORD *out_value);
static int psd_read_u32_be(IStream *stream, DWORD *out_value);
static int psd_skip_bytes(IStream *stream, DWORD size);
static int psd_read_header(IStream *stream, _viv_psd_t *psd);
static int psd_read_color_mode_data(IStream *stream, _viv_psd_t *psd);
static int psd_read_image_resources(IStream *stream, _viv_psd_t *psd);
static int psd_read_raw_image_data(IStream *stream, const _viv_psd_t *psd, BYTE *plane_data);
static int psd_read_rle_image_data(IStream *stream, const _viv_psd_t *psd, BYTE *plane_data);
static int psd_read_zip_image_data(IStream *stream, const _viv_psd_t *psd, BYTE *plane_data);
static int psd_normalize_to_bgra8(const _viv_psd_t *psd, const BYTE *plane_data, BYTE *dst_bgra);
static int psd_decode_packbits_row(IStream *stream, BYTE *dst, DWORD row_width, DWORD encoded_size);
```

目标是把解析逻辑尽量封装在 `src/psd.c` 内部，避免把 `src/viv.c` 进一步膨胀。

## 10. `psd_load()` 建议流程

伪代码如下：

```c
int psd_load(IStream *stream, void *user_data, info_cb, frame_cb)
{
    _viv_psd_t psd;
    BYTE *plane_data;
    BYTE *pixels;
    int has_alpha;

    if (!psd_read_header(stream, &psd)) {
        return 0;
    }

    if (!psd_read_color_mode_data(stream, &psd)) {
        return 0;
    }

    if (!psd_read_image_resources(stream, &psd)) {
        return 0;
    }

    if (!psd_skip_layer_and_mask_info(stream)) {
        return 0;
    }

    if (!psd_read_compression(stream, &psd.compression)) {
        return 0;
    }

    if (psd.compression > 3) {
        return 0;
    }

    plane_data = mem_alloc(...);
    pixels = mem_alloc(psd.width * psd.height * 4);
    if (!plane_data || !pixels) {
        return 0;
    }

    if (psd.compression == 0) {
        if (!psd_read_raw_image_data(stream, &psd, plane_data)) {
            mem_free(plane_data);
            mem_free(pixels);
            return 0;
        }
    } else if (psd.compression == 1) {
        if (!psd_read_rle_image_data(stream, &psd, plane_data)) {
            mem_free(plane_data);
            mem_free(pixels);
            return 0;
        }
    } else {
        if (!psd_read_zip_image_data(stream, &psd, plane_data)) {
            mem_free(plane_data);
            mem_free(pixels);
            return 0;
        }
    }

    if (!psd_normalize_to_bgra8(&psd, plane_data, pixels)) {
        mem_free(plane_data);
        mem_free(pixels);
        return 0;
    }

    has_alpha = psd_has_alpha(&psd);

    if (!info_cb(user_data, 1, psd.width, psd.height, has_alpha)) {
        mem_free(plane_data);
        mem_free(pixels);
        return 0;
    }

    if (!frame_cb(user_data, pixels, 0)) {
        mem_free(plane_data);
        mem_free(pixels);
        return 0;
    }

    mem_free(plane_data);
    mem_free(pixels);
    return 1;
}
```

关键点：

- 对 PSD 来说，`frame_count` 固定传 `1`
- `delay` 固定传 `0`
- 所有失败路径都必须释放临时缓冲

## 11. 接入 `voidImageViewer` 的推荐方式

推荐与当前 `webp_load()` 保持同样的接入姿势：

1. `src/viv.h` 中引入 `psd.h`
2. `src/viv.c` 在 `GDI+` 失败后尝试 `psd_load(stream, ...)`
3. 若 `psd_load()` 返回成功，就走现有第一帧位图回调链
4. 若失败，再继续最终报错

建议顺序：

```text
GDI+ -> PSD -> WEBP -> fail
```

原因：

- `.webp` 目前本就是“GDI+ 打不开时的专用格式”
- `.psd` 也属于这一类
- 两者互不冲突，按专用格式逐个尝试即可

## 12. 与现有回调链的映射

当前项目已经有两套很关键的回调：

- `info_callback`
- `frame_callback`

PSD 对应关系很简单：

- `frame_count = 1`
- `width = header.width`
- `height = header.height`
- `has_alpha = psd_has_alpha(&psd)`
- `delay = 0`

因此 PSD 不需要额外定义新的帧模型，直接复用现有单帧路径即可。

## 13. 错误处理建议

首版不要做模糊容错，建议遇到以下情况直接失败：

- 不是 `8BPS`
- `version != 1`
- `depth != 8`
- `color_mode` 不在支持集合
- `channels` 与颜色模式不匹配
- `compression` 不在 `0..3`
- 任意长度字段导致越界或 seek 失败
- RLE 某行解码后长度不等于 `width`
- ZIP 解压或 prediction 还原失败
- ICC 资源损坏导致颜色转换失败
- 内存预算超限

这种失败策略比“勉强显示但颜色错乱”更适合看图器。

## 14. 内存与尺寸保护

PSD 常见于设计稿，大图非常多。建议在申请像素缓冲前做一次预算检查：

```text
pixel_bytes = width * height * 4
channel_bytes = width * height
```

最少做两层保护：

- 使用现有安全大小计算工具，避免整数溢出
- 给首版设一个合理上限，超过就失败

否则很容易在异常 PSD 或超大 PSD 上触发分配失败或溢出。

## 15. 推荐测试样本

最低样本集建议：

1. `grayscale-8bit.psd`
   预期：正常显示
2. `indexed-8bit.psd`
   预期：调色板映射正确
3. `rgb-8bit-flat.psd`
   预期：正常显示
4. `rgba-8bit-transparent.psd`
   预期：透明区域显示正常
5. `rgb-8bit-rle.psd`
   预期：RLE 解码正确
6. `cmyk-8bit.psd`
   预期：颜色转换正确
7. `lab-8bit.psd`
   预期：颜色转换正确
8. `duotone-8bit.psd`
   预期：正常显示
9. `multichannel-8bit.psd`
   预期：正常显示
10. `zip-8bit.psd`
   预期：ZIP 解压正确
11. `zip-predict-8bit.psd`
   预期：prediction 还原正确
12. `rgb-16bit.psd`
   预期：明确失败
13. `very-large-rgb.psd`
   预期：要么正常显示，要么因超限安全失败

如果能准备一张“关闭兼容性合成图”的 PSD，也建议加入测试，确认“所有 8-bit PSD”在当前实现边界内是否仍成立。

## 16. 开工顺序建议

推荐按这个顺序做，而不是一上来改 UI：

1. 先在 `src/psd.c` 做一个最小 PSD 容器 spike
2. 先验证 `Indexed / CMYK / Lab` 三类最容易做错颜色的样本
3. 再补齐四种压缩
4. 再接入 `src/viv.c`
5. 最后补扩展名、关联、README 和帮助文本

这样能把调试范围控制在最小。

## 17. 与主计划的关系

这份文档对应主计划中的两个阶段：

- 阶段 1：选型与样本验证
- 阶段 2：接入 PSD 解码模块

执行时建议这样配合：

- 用 [PSD 支持计划](2026-04-01-psd-support-plan.md) 管阶段和交付
- 用本文件管实现细节和失败边界
