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

#include "viv.h"

#define _COLOR_MAX_PROFILE_SIZE (64U * 1024U * 1024U)

static int _color_get_profile_path_for_srgb(wchar_t *path,DWORD path_size)
{
	DWORD size;
	
	if ((!path) || (!path_size))
	{
		return 0;
	}
	
	path[0] = 0;
	
	if (!os_GetStandardColorSpaceProfileW)
	{
		return 0;
	}
	
	size = path_size;
	
	return os_GetStandardColorSpaceProfileW(NULL,LCS_sRGB,path,&size) ? 1 : 0;
}

static HPROFILE _color_open_profile_from_blob(const color_profile_t *profile)
{
	PROFILE open_profile;
	
	if ((!profile) || (!profile->data) || (!profile->size) || (!os_OpenColorProfileW))
	{
		return NULL;
	}
	
	open_profile.dwType = PROFILE_MEMBUFFER;
	open_profile.pProfileData = profile->data;
	open_profile.cbDataSize = profile->size;
	
	return os_OpenColorProfileW(&open_profile,PROFILE_READ,FILE_SHARE_READ,OPEN_EXISTING);
}

static HPROFILE _color_open_profile_from_path(const wchar_t *profile_path)
{
	PROFILE open_profile;
	
	if ((!profile_path) || (!*profile_path) || (!os_OpenColorProfileW))
	{
		return NULL;
	}
	
	open_profile.dwType = PROFILE_FILENAME;
	open_profile.pProfileData = (void *)profile_path;
	open_profile.cbDataSize = ((DWORD)string_get_length(profile_path) + 1) * sizeof(wchar_t);
	
	return os_OpenColorProfileW(&open_profile,PROFILE_READ,FILE_SHARE_READ,OPEN_EXISTING);
}

static int _color_copy_pixels(const BYTE *src_pixels,BYTE *dst_pixels,DWORD wide,DWORD high)
{
	SIZE_T size;
	
	if ((!src_pixels) || (!dst_pixels))
	{
		return 0;
	}
	
	if (!color_get_bgra_size(wide,high,&size))
	{
		return 0;
	}
	
	if (src_pixels != dst_pixels)
	{
		os_copy_memory(dst_pixels,src_pixels,(int)size);
	}
	
	return 1;
}

static int _color_transform_bgra_internal(const BYTE *src_pixels,BYTE *dst_pixels,DWORD wide,DWORD high,const color_profile_t *source_profile,const wchar_t *source_profile_path,const wchar_t *destination_profile_path,int destination_is_srgb)
{
	HTRANSFORM transform;
	HPROFILE source_hprofile;
	HPROFILE destination_hprofile;
	wchar_t srgb_profile_path[STRING_SIZE];
	const wchar_t *resolved_source_profile_path;
	const wchar_t *resolved_destination_profile_path;
	DWORD intents[2];
	HPROFILE profiles[2];
	SIZE_T bitmap_size;
	BYTE *temp_pixels;
	int ret;
	
	ret = 0;
	transform = NULL;
	source_hprofile = NULL;
	destination_hprofile = NULL;
	resolved_source_profile_path = source_profile_path;
	resolved_destination_profile_path = destination_profile_path;
	temp_pixels = NULL;
	srgb_profile_path[0] = 0;
	
	if ((!src_pixels) || (!dst_pixels))
	{
		return 0;
	}
	
	if (!color_get_bgra_size(wide,high,&bitmap_size))
	{
		return 0;
	}
	
	if ((!source_profile) || (!source_profile->data) || (!source_profile->size))
	{
		if ((!resolved_source_profile_path) || (!*resolved_source_profile_path))
		{
			if (_color_get_profile_path_for_srgb(srgb_profile_path,STRING_SIZE))
			{
				resolved_source_profile_path = srgb_profile_path;
			}
		}
	}
	
	if (destination_is_srgb)
	{
		if (!*srgb_profile_path)
		{
			_color_get_profile_path_for_srgb(srgb_profile_path,STRING_SIZE);
		}
		
		resolved_destination_profile_path = srgb_profile_path;
	}
	
	if ((!resolved_destination_profile_path) || (!*resolved_destination_profile_path))
	{
		return _color_copy_pixels(src_pixels,dst_pixels,wide,high);
	}
	
	if ((resolved_source_profile_path) && (*resolved_source_profile_path) && (string_compare(resolved_source_profile_path,resolved_destination_profile_path) == 0))
	{
		return _color_copy_pixels(src_pixels,dst_pixels,wide,high);
	}

	source_hprofile = _color_open_profile_from_blob(source_profile);
	if (!source_hprofile)
	{
		source_hprofile = _color_open_profile_from_path(resolved_source_profile_path);
	}
	
	destination_hprofile = _color_open_profile_from_path(resolved_destination_profile_path);
	
	if ((!source_hprofile) || (!destination_hprofile) || (!os_CreateMultiProfileTransform) || (!os_TranslateBitmapBits))
	{
		ret = _color_copy_pixels(src_pixels,dst_pixels,wide,high);
		goto end;
	}
	
	profiles[0] = source_hprofile;
	profiles[1] = destination_hprofile;
	intents[0] = INTENT_PERCEPTUAL;
	intents[1] = INTENT_PERCEPTUAL;
	
	transform = os_CreateMultiProfileTransform(profiles,2,intents,2,0,INDEX_DONT_CARE);
	if (!transform)
	{
		ret = _color_copy_pixels(src_pixels,dst_pixels,wide,high);
		goto end;
	}
	
	if (src_pixels == dst_pixels)
	{
		temp_pixels = (BYTE *)mem_alloc(bitmap_size);
		if (!temp_pixels)
		{
			ret = 0;
			goto end;
		}
		
		dst_pixels = temp_pixels;
	}
	
	if (os_TranslateBitmapBits(transform,(void *)src_pixels,BM_xRGBQUADS,wide,high,wide * 4,dst_pixels,BM_xRGBQUADS,wide * 4,NULL,0))
	{
		if (temp_pixels)
		{
			os_copy_memory((BYTE *)src_pixels,temp_pixels,(int)bitmap_size);
		}
		
		ret = 1;
	}
	else
	{
		ret = _color_copy_pixels(src_pixels,(temp_pixels ? (BYTE *)src_pixels : dst_pixels),wide,high);
	}
	
end:
	if (transform)
	{
		os_DeleteColorTransform(transform);
	}
	
	if (destination_hprofile)
	{
		os_CloseColorProfile(destination_hprofile);
	}
	
	if (source_hprofile)
	{
		os_CloseColorProfile(source_hprofile);
	}
	
	if (temp_pixels)
	{
		mem_free(temp_pixels);
	}
	
	return ret;
}

void color_profile_init(color_profile_t *profile)
{
	if (profile)
	{
		profile->data = NULL;
		profile->size = 0;
	}
}

void color_profile_clear(color_profile_t *profile)
{
	if (profile)
	{
		if (profile->data)
		{
			mem_free(profile->data);
			profile->data = NULL;
		}
		
		profile->size = 0;
	}
}

int color_icm_is_active(void)
{
	return (os_OpenColorProfileW) && (os_CloseColorProfile) && (os_CreateMultiProfileTransform) && (os_DeleteColorTransform) && (os_TranslateBitmapBits) && (os_GetStandardColorSpaceProfileW);
}

int color_profile_set(color_profile_t *profile,const BYTE *data,DWORD size)
{
	BYTE *new_data;
	
	if (!profile)
	{
		return 0;
	}
	
	color_profile_clear(profile);
	
	if ((!data) || (!size))
	{
		return 1;
	}

	if ((size > _COLOR_MAX_PROFILE_SIZE) || (size > 0x7fffffffU))
	{
		return 0;
	}
	
	new_data = (BYTE *)mem_alloc(size);
	if (!new_data)
	{
		return 0;
	}
	
	__try
	{
		os_copy_memory(new_data,data,(int)size);
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		mem_free(new_data);
		return 0;
	}
	
	profile->data = new_data;
	profile->size = size;
	return 1;
}

int color_get_bgra_size(DWORD wide,DWORD high,SIZE_T *out_size)
{
	SIZE_T pixel_count;
	SIZE_T total_size;
	
	if (!out_size)
	{
		return 0;
	}
	
	pixel_count = safe_size_mul(wide,high);
	if (pixel_count == SIZE_MAX)
	{
		return 0;
	}
	
	total_size = safe_size_mul(pixel_count,4);
	if (total_size == SIZE_MAX)
	{
		return 0;
	}
	
	*out_size = total_size;
	return 1;
}

int color_copy_hbitmap_to_bgra(HBITMAP hbitmap,DWORD wide,DWORD high,BYTE **out_pixels)
{
	BITMAPINFO bmi;
	BYTE *pixels;
	SIZE_T bitmap_size;
	HDC screen_hdc;
	HDC mem_hdc;
	int ret;
	
	if ((!hbitmap) || (!out_pixels))
	{
		return 0;
	}
	
	if (!color_get_bgra_size(wide,high,&bitmap_size))
	{
		return 0;
	}
	
	pixels = (BYTE *)mem_alloc(bitmap_size);
	if (!pixels)
	{
		return 0;
	}
	
	os_zero_memory(&bmi,sizeof(BITMAPINFO));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = wide;
	bmi.bmiHeader.biHeight = -(int)high;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	
	ret = 0;
	screen_hdc = GetDC(NULL);
	if (screen_hdc)
	{
		mem_hdc = CreateCompatibleDC(screen_hdc);
		if (mem_hdc)
		{
			if (GetDIBits(mem_hdc,hbitmap,0,high,pixels,&bmi,DIB_RGB_COLORS))
			{
				*out_pixels = pixels;
				ret = 1;
			}
			
			DeleteDC(mem_hdc);
		}
		
		ReleaseDC(NULL,screen_hdc);
	}
	
	if (!ret)
	{
		mem_free(pixels);
	}
	
	return ret;
}

HBITMAP color_create_hbitmap_from_bgra(DWORD wide,DWORD high,const BYTE *pixels)
{
	BITMAPINFO bmi;
	HBITMAP hbitmap;
	HDC screen_hdc;
	HDC mem_hdc;
	
	if ((!wide) || (!high) || (!pixels))
	{
		return NULL;
	}
	
	os_zero_memory(&bmi,sizeof(BITMAPINFO));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = wide;
	bmi.bmiHeader.biHeight = -(int)high;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	
	hbitmap = NULL;
	screen_hdc = GetDC(NULL);
	if (screen_hdc)
	{
		mem_hdc = CreateCompatibleDC(screen_hdc);
		if (mem_hdc)
		{
			hbitmap = CreateCompatibleBitmap(screen_hdc,wide,high);
			if (hbitmap)
			{
				if (!SetDIBits(mem_hdc,hbitmap,0,high,pixels,&bmi,DIB_RGB_COLORS))
				{
					DeleteObject(hbitmap);
					hbitmap = NULL;
				}
			}
			
			DeleteDC(mem_hdc);
		}
		
		ReleaseDC(NULL,screen_hdc);
	}
	
	return hbitmap;
}

int color_transform_to_srgb(BYTE *pixels,DWORD wide,DWORD high,const color_profile_t *source_profile,const wchar_t *source_profile_path)
{
	if (!color_icm_is_active())
	{
		return _color_copy_pixels(pixels,pixels,wide,high);
	}

	return _color_transform_bgra_internal(pixels,pixels,wide,high,source_profile,source_profile_path,NULL,1);
}

int color_transform_srgb_to_display(const BYTE *src_pixels,BYTE *dst_pixels,DWORD wide,DWORD high,const wchar_t *display_profile_path)
{
	if (!color_icm_is_active())
	{
		return _color_copy_pixels(src_pixels,dst_pixels,wide,high);
	}

	return _color_transform_bgra_internal(src_pixels,dst_pixels,wide,high,NULL,NULL,display_profile_path,0);
}

void color_copy_rgba_to_bgra(const BYTE *src_pixels,BYTE *dst_pixels,DWORD wide,DWORD high)
{
	DWORD run;
	
	if ((!src_pixels) || (!dst_pixels))
	{
		return;
	}
	
	run = wide * high;
	while(run)
	{
		dst_pixels[0] = src_pixels[2];
		dst_pixels[1] = src_pixels[1];
		dst_pixels[2] = src_pixels[0];
		dst_pixels[3] = src_pixels[3];
		
		src_pixels += 4;
		dst_pixels += 4;
		run--;
	}
}

void color_flatten_bgra(BYTE *pixels,DWORD wide,DWORD high,COLORREF background_color)
{
	DWORD run;
	BYTE background_red;
	BYTE background_green;
	BYTE background_blue;
	
	if (!pixels)
	{
		return;
	}
	
	background_red = GetRValue(background_color);
	background_green = GetGValue(background_color);
	background_blue = GetBValue(background_color);
	
	run = wide * high;
	while(run)
	{
		BYTE alpha;
		
		alpha = pixels[3];
		if (alpha != 255)
		{
			pixels[0] = background_blue + ((pixels[0] - background_blue) * alpha) / 255;
			pixels[1] = background_green + ((pixels[1] - background_green) * alpha) / 255;
			pixels[2] = background_red + ((pixels[2] - background_red) * alpha) / 255;
			pixels[3] = 255;
		}
		
		pixels += 4;
		run--;
	}
}

int color_orient_bgra(BYTE **pixels,DWORD *wide,DWORD *high,int orientation)
{
	BYTE *old_pixels;
	BYTE *new_pixels;
	DWORD old_wide;
	DWORD old_high;
	DWORD new_wide;
	DWORD new_high;
	SIZE_T new_size;
	DWORD x;
	DWORD y;
	
	if ((!pixels) || (!*pixels) || (!wide) || (!high) || (orientation <= 1))
	{
		return 1;
	}
	
	old_pixels = *pixels;
	old_wide = *wide;
	old_high = *high;
	new_wide = old_wide;
	new_high = old_high;
	
	switch(orientation)
	{
		case 5:
		case 6:
		case 7:
		case 8:
			new_wide = old_high;
			new_high = old_wide;
			break;
	}
	
	if (!color_get_bgra_size(new_wide,new_high,&new_size))
	{
		return 0;
	}
	
	new_pixels = (BYTE *)mem_alloc(new_size);
	if (!new_pixels)
	{
		return 0;
	}
	
	for(y=0;y<new_high;y++)
	{
		for(x=0;x<new_wide;x++)
		{
			DWORD src_x;
			DWORD src_y;
			BYTE *dst_pixel;
			BYTE *src_pixel;
			
			src_x = x;
			src_y = y;
			
			switch(orientation)
			{
				case 2:
					src_x = old_wide - x - 1;
					break;
					
				case 3:
					src_x = old_wide - x - 1;
					src_y = old_high - y - 1;
					break;
					
				case 4:
					src_y = old_high - y - 1;
					break;
					
				case 5:
					src_x = y;
					src_y = x;
					break;
					
				case 6:
					src_x = y;
					src_y = old_high - x - 1;
					break;
					
				case 7:
					src_x = old_wide - y - 1;
					src_y = old_high - x - 1;
					break;
					
				case 8:
					src_x = old_wide - y - 1;
					src_y = x;
					break;
			}
			
			dst_pixel = new_pixels + ((x + (y * new_wide)) * 4);
			src_pixel = old_pixels + ((src_x + (src_y * old_wide)) * 4);
			os_copy_memory(dst_pixel,src_pixel,4);
		}
	}
	
	mem_free(old_pixels);
	*pixels = new_pixels;
	*wide = new_wide;
	*high = new_high;
	return 1;
}
