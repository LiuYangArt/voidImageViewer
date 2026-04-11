//
// Copyright 2026 voidtools / David Carpenter
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// color management helpers

#ifdef __cplusplus
extern "C" {
#endif

typedef struct color_profile_s
{
	BYTE *data;
	DWORD size;

} color_profile_t;

void color_profile_init(color_profile_t *profile);
void color_profile_clear(color_profile_t *profile);
int color_profile_set(color_profile_t *profile,const BYTE *data,DWORD size);
int color_icm_is_active(void);
int color_get_bgra_size(DWORD wide,DWORD high,SIZE_T *out_size);
int color_copy_hbitmap_to_bgra(HBITMAP hbitmap,DWORD wide,DWORD high,BYTE **out_pixels);
HBITMAP color_create_hbitmap_from_bgra(DWORD wide,DWORD high,const BYTE *pixels);
int color_transform_to_srgb(BYTE *pixels,DWORD wide,DWORD high,const color_profile_t *source_profile,const wchar_t *source_profile_path);
int color_transform_srgb_to_display(const BYTE *src_pixels,BYTE *dst_pixels,DWORD wide,DWORD high,const wchar_t *display_profile_path);
void color_clear_transform_cache(void);
void color_copy_rgba_to_bgra(const BYTE *src_pixels,BYTE *dst_pixels,DWORD wide,DWORD high);
void color_flatten_bgra(BYTE *pixels,DWORD wide,DWORD high,COLORREF background_color);
int color_orient_bgra(BYTE **pixels,DWORD *wide,DWORD *high,int orientation);

#ifdef __cplusplus
}
#endif
