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
// psd layer

#include "viv.h"
#include "miniz/miniz.h"
#include <math.h>

#define PSD_COLOR_MODE_BITMAP			0
#define PSD_COLOR_MODE_GRAYSCALE		1
#define PSD_COLOR_MODE_INDEXED			2
#define PSD_COLOR_MODE_RGB				3
#define PSD_COLOR_MODE_CMYK				4
#define PSD_COLOR_MODE_MULTICHANNEL		7
#define PSD_COLOR_MODE_DUOTONE			8
#define PSD_COLOR_MODE_LAB				9

#define PSD_RESOURCE_ID_ICC_PROFILE			1039
#define PSD_RESOURCE_ID_TRANSPARENCY_INDEX	1047

typedef struct _viv_psd_s
{
	const BYTE *data;
	SIZE_T size;
	SIZE_T offset;
	DWORD width;
	DWORD height;
	WORD channels;
	WORD depth;
	WORD color_mode;
	WORD compression;
	DWORD color_channels;
	int has_alpha;
	const BYTE *palette;
	DWORD palette_size;
	int has_transparency_index;
	BYTE transparency_index;
	
}_viv_psd_t;

static int psd_require_available(const _viv_psd_t *psd,SIZE_T size);
static int psd_read_bytes(_viv_psd_t *psd,SIZE_T size,const BYTE **out_data);
static int psd_skip_bytes(_viv_psd_t *psd,SIZE_T size);
static int psd_read_u16_be(_viv_psd_t *psd,WORD *out_value);
static int psd_read_u32_be(_viv_psd_t *psd,DWORD *out_value);
static DWORD psd_get_color_channels(WORD color_mode,WORD channels);
static int psd_read_header(_viv_psd_t *psd);
static int psd_read_color_mode_data(_viv_psd_t *psd);
static int psd_skip_pascal_string(_viv_psd_t *psd);
static int psd_read_image_resources(_viv_psd_t *psd);
static int psd_skip_layer_mask_info(_viv_psd_t *psd);
static int psd_get_pixel_count(const _viv_psd_t *psd,SIZE_T *out_pixel_count);
static int psd_get_plane_data_size(const _viv_psd_t *psd,SIZE_T *out_size);
static int psd_decode_packbits_row(const BYTE *src,SIZE_T src_size,BYTE *dst,DWORD row_width);
static int psd_read_raw_image_data(_viv_psd_t *psd,BYTE *plane_data,SIZE_T plane_data_size);
static int psd_read_rle_image_data(_viv_psd_t *psd,BYTE *plane_data,SIZE_T plane_data_size);
static int psd_apply_zip_prediction(const _viv_psd_t *psd,BYTE *plane_data,SIZE_T plane_data_size);
static int psd_read_zip_image_data(_viv_psd_t *psd,BYTE *plane_data,SIZE_T plane_data_size,int with_prediction);
static int psd_read_compressed_image_data(_viv_psd_t *psd,BYTE *plane_data,SIZE_T plane_data_size);
static int psd_get_alpha_plane_index(const _viv_psd_t *psd);
static BYTE psd_get_alpha_value(const _viv_psd_t *psd,const BYTE *plane_data,SIZE_T pixel_count,SIZE_T pixel_index);
static BYTE psd_clamp_byte(int value);
static BYTE psd_clamp_double_to_byte(double value);
static void psd_cmyk_to_rgb(int cyan,int magenta,int yellow,int black,BYTE *out_red,BYTE *out_green,BYTE *out_blue);
static void psd_lab_to_rgb(int lightness,int a,int b,BYTE *out_red,BYTE *out_green,BYTE *out_blue);
static int psd_normalize_to_rgba8(const _viv_psd_t *psd,const BYTE *plane_data,BYTE *dst_pixels);

static int psd_require_available(const _viv_psd_t *psd,SIZE_T size)
{
	if (psd->offset > psd->size)
	{
		return 0;
	}
	
	return size <= psd->size - psd->offset;
}

static int psd_read_bytes(_viv_psd_t *psd,SIZE_T size,const BYTE **out_data)
{
	if (!psd_require_available(psd,size))
	{
		return 0;
	}
	
	*out_data = psd->data + psd->offset;
	psd->offset += size;
	return 1;
}

static int psd_skip_bytes(_viv_psd_t *psd,SIZE_T size)
{
	if (!psd_require_available(psd,size))
	{
		return 0;
	}
	
	psd->offset += size;
	return 1;
}

static int psd_read_u16_be(_viv_psd_t *psd,WORD *out_value)
{
	const BYTE *p;
	
	if (!psd_read_bytes(psd,2,&p))
	{
		return 0;
	}
	
	*out_value = ((WORD)p[0] << 8) | p[1];
	return 1;
}

static int psd_read_u32_be(_viv_psd_t *psd,DWORD *out_value)
{
	const BYTE *p;
	
	if (!psd_read_bytes(psd,4,&p))
	{
		return 0;
	}
	
	*out_value = ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) | ((DWORD)p[2] << 8) | p[3];
	return 1;
}

static DWORD psd_get_color_channels(WORD color_mode,WORD channels)
{
	switch (color_mode)
	{
		case PSD_COLOR_MODE_GRAYSCALE:
		case PSD_COLOR_MODE_INDEXED:
		case PSD_COLOR_MODE_DUOTONE:
			return 1;
			
		case PSD_COLOR_MODE_RGB:
		case PSD_COLOR_MODE_LAB:
			return 3;
			
		case PSD_COLOR_MODE_CMYK:
			return 4;
			
		case PSD_COLOR_MODE_MULTICHANNEL:
			if (channels >= 4)
			{
				return 4;
			}
			
			if (channels >= 3)
			{
				return 3;
			}
			
			return 0;
	}
	
	return 0;
}

static int psd_read_header(_viv_psd_t *psd)
{
	const BYTE *signature;
	const BYTE *reserved;
	WORD version;
	
	if (!psd_read_bytes(psd,4,&signature))
	{
		return 0;
	}
	
	if ((signature[0] != '8') || (signature[1] != 'B') || (signature[2] != 'P') || (signature[3] != 'S'))
	{
		return 0;
	}
	
	if (!psd_read_u16_be(psd,&version))
	{
		return 0;
	}
	
	if (version != 1)
	{
		return 0;
	}
	
	if (!psd_read_bytes(psd,6,&reserved))
	{
		return 0;
	}
	
	if ((reserved[0] != 0) || (reserved[1] != 0) || (reserved[2] != 0) || (reserved[3] != 0) || (reserved[4] != 0) || (reserved[5] != 0))
	{
		return 0;
	}
	
	if (!psd_read_u16_be(psd,&psd->channels))
	{
		return 0;
	}
	
	if (!psd_read_u32_be(psd,&psd->height))
	{
		return 0;
	}
	
	if (!psd_read_u32_be(psd,&psd->width))
	{
		return 0;
	}
	
	if (!psd_read_u16_be(psd,&psd->depth))
	{
		return 0;
	}
	
	if (!psd_read_u16_be(psd,&psd->color_mode))
	{
		return 0;
	}
	
	if ((!psd->width) || (!psd->height))
	{
		return 0;
	}
	
	if (psd->depth != 8)
	{
		return 0;
	}
	
	psd->color_channels = psd_get_color_channels(psd->color_mode,psd->channels);
	if (!psd->color_channels)
	{
		return 0;
	}
	
	if (psd->channels < psd->color_channels)
	{
		return 0;
	}
	
	psd->has_alpha = psd->channels > psd->color_channels;
	return 1;
}

static int psd_read_color_mode_data(_viv_psd_t *psd)
{
	DWORD length;
	const BYTE *data;
	
	if (!psd_read_u32_be(psd,&length))
	{
		return 0;
	}
	
	if (!psd_read_bytes(psd,length,&data))
	{
		return 0;
	}
	
	if (psd->color_mode == PSD_COLOR_MODE_INDEXED)
	{
		if (length < 768)
		{
			return 0;
		}
		
		psd->palette = data;
		psd->palette_size = 256;
	}
	
	return 1;
}

static int psd_skip_pascal_string(_viv_psd_t *psd)
{
	const BYTE *length_ptr;
	SIZE_T total_length;
	
	if (!psd_read_bytes(psd,1,&length_ptr))
	{
		return 0;
	}
	
	total_length = (SIZE_T)length_ptr[0];
	if (!psd_skip_bytes(psd,total_length))
	{
		return 0;
	}
	
	if (((total_length + 1) & 1) != 0)
	{
		if (!psd_skip_bytes(psd,1))
		{
			return 0;
		}
	}
	
	return 1;
}

static int psd_read_image_resources(_viv_psd_t *psd)
{
	DWORD length;
	SIZE_T end_offset;
	
	if (!psd_read_u32_be(psd,&length))
	{
		return 0;
	}
	
	end_offset = psd->offset + length;
	if (end_offset < psd->offset)
	{
		return 0;
	}
	
	if (end_offset > psd->size)
	{
		return 0;
	}
	
	while (psd->offset < end_offset)
	{
		const BYTE *signature;
		WORD resource_id;
		DWORD size;
		const BYTE *resource_data;
		
		if (!psd_read_bytes(psd,4,&signature))
		{
			return 0;
		}
		
		if ((signature[0] != '8') || (signature[1] != 'B') || (signature[2] != 'I') || (signature[3] != 'M'))
		{
			return 0;
		}
		
		if (!psd_read_u16_be(psd,&resource_id))
		{
			return 0;
		}
		
		if (!psd_skip_pascal_string(psd))
		{
			return 0;
		}
		
		if (!psd_read_u32_be(psd,&size))
		{
			return 0;
		}
		
		if (!psd_read_bytes(psd,size,&resource_data))
		{
			return 0;
		}
		
		if ((resource_id == PSD_RESOURCE_ID_TRANSPARENCY_INDEX) && (size >= 2))
		{
			WORD transparency_index;
			
			transparency_index = ((WORD)resource_data[0] << 8) | resource_data[1];
			if (transparency_index <= 255)
			{
				psd->has_transparency_index = 1;
				psd->transparency_index = (BYTE)transparency_index;
			}
		}
		else
		if (resource_id == PSD_RESOURCE_ID_ICC_PROFILE)
		{
			/* Parsed only so the section walk stays correct for future color-management work. */
		}
		
		if (size & 1)
		{
			if (!psd_skip_bytes(psd,1))
			{
				return 0;
			}
		}
	}
	
	return psd->offset == end_offset;
}

static int psd_skip_layer_mask_info(_viv_psd_t *psd)
{
	DWORD length;
	
	if (!psd_read_u32_be(psd,&length))
	{
		return 0;
	}
	
	return psd_skip_bytes(psd,length);
}

static int psd_get_pixel_count(const _viv_psd_t *psd,SIZE_T *out_pixel_count)
{
	SIZE_T pixel_count;
	
	pixel_count = safe_size_mul(psd->width,psd->height);
	if (pixel_count == SIZE_MAX)
	{
		return 0;
	}
	
	*out_pixel_count = pixel_count;
	return 1;
}

static int psd_get_plane_data_size(const _viv_psd_t *psd,SIZE_T *out_size)
{
	SIZE_T pixel_count;
	SIZE_T plane_data_size;
	
	if (!psd_get_pixel_count(psd,&pixel_count))
	{
		return 0;
	}
	
	plane_data_size = safe_size_mul(pixel_count,psd->channels);
	if (plane_data_size == SIZE_MAX)
	{
		return 0;
	}
	
	*out_size = plane_data_size;
	return 1;
}

static int psd_decode_packbits_row(const BYTE *src,SIZE_T src_size,BYTE *dst,DWORD row_width)
{
	SIZE_T src_offset;
	DWORD dst_offset;
	
	src_offset = 0;
	dst_offset = 0;
	
	while ((dst_offset < row_width) && (src_offset < src_size))
	{
		BYTE code;
		
		code = src[src_offset];
		src_offset++;
		
		if (code <= 127)
		{
			DWORD count;
			
			count = (DWORD)code + 1;
			if ((SIZE_T)count > src_size - src_offset)
			{
				return 0;
			}
			
			if (count > row_width - dst_offset)
			{
				return 0;
			}
			
			memcpy(dst + dst_offset,src + src_offset,count);
			src_offset += count;
			dst_offset += count;
		}
		else
		if (code >= 129)
		{
			DWORD count;
			
			if (src_offset >= src_size)
			{
				return 0;
			}
			
			count = 257 - code;
			if (count > row_width - dst_offset)
			{
				return 0;
			}
			
			memset(dst + dst_offset,src[src_offset],count);
			src_offset++;
			dst_offset += count;
		}
	}
	
	return dst_offset == row_width;
}

static int psd_read_raw_image_data(_viv_psd_t *psd,BYTE *plane_data,SIZE_T plane_data_size)
{
	const BYTE *data;
	
	if (!psd_read_bytes(psd,plane_data_size,&data))
	{
		return 0;
	}
	
	memcpy(plane_data,data,plane_data_size);
	return 1;
}

static int psd_read_rle_image_data(_viv_psd_t *psd,BYTE *plane_data,SIZE_T plane_data_size)
{
	const BYTE *counts_data;
	SIZE_T pixel_count;
	SIZE_T counts_size;
	DWORD channel_index;
	DWORD y;
	SIZE_T count_index;
	
	if (!psd_get_pixel_count(psd,&pixel_count))
	{
		return 0;
	}
	
	counts_size = safe_size_mul(psd->channels,psd->height);
	if (counts_size == SIZE_MAX)
	{
		return 0;
	}
	
	counts_size = safe_size_mul(counts_size,2);
	if (counts_size == SIZE_MAX)
	{
		return 0;
	}
	
	if (!psd_read_bytes(psd,counts_size,&counts_data))
	{
		return 0;
	}
	
	count_index = 0;
	for(channel_index=0;channel_index<psd->channels;channel_index++)
	{
		for(y=0;y<psd->height;y++)
		{
			WORD row_size;
			const BYTE *row_data;
			BYTE *dst_row;
			
			if (count_index + 1 >= counts_size)
			{
				return 0;
			}
			
			row_size = ((WORD)counts_data[count_index] << 8) | counts_data[count_index + 1];
			count_index += 2;
			
			if (!psd_read_bytes(psd,row_size,&row_data))
			{
				return 0;
			}
			
			dst_row = plane_data + (pixel_count * channel_index) + ((SIZE_T)y * psd->width);
			if (!psd_decode_packbits_row(row_data,row_size,dst_row,psd->width))
			{
				return 0;
			}
		}
	}
	
	return plane_data_size == pixel_count * psd->channels;
}

static int psd_apply_zip_prediction(const _viv_psd_t *psd,BYTE *plane_data,SIZE_T plane_data_size)
{
	SIZE_T pixel_count;
	DWORD channel_index;
	DWORD y;
	
	if (!psd_get_pixel_count(psd,&pixel_count))
	{
		return 0;
	}
	
	if (plane_data_size != pixel_count * psd->channels)
	{
		return 0;
	}
	
	for(channel_index=0;channel_index<psd->channels;channel_index++)
	{
		for(y=0;y<psd->height;y++)
		{
			BYTE *row;
			DWORD x;
			
			row = plane_data + (pixel_count * channel_index) + ((SIZE_T)y * psd->width);
			for(x=1;x<psd->width;x++)
			{
				row[x] = (BYTE)(row[x] + row[x - 1]);
			}
		}
	}
	
	return 1;
}

static int psd_read_zip_image_data(_viv_psd_t *psd,BYTE *plane_data,SIZE_T plane_data_size,int with_prediction)
{
	const BYTE *compressed_data;
	SIZE_T compressed_size;
	size_t result_size;
	
	if (!psd_require_available(psd,0))
	{
		return 0;
	}
	
	compressed_data = psd->data + psd->offset;
	compressed_size = psd->size - psd->offset;
	result_size = tinfl_decompress_mem_to_mem(plane_data,plane_data_size,compressed_data,compressed_size,TINFL_FLAG_PARSE_ZLIB_HEADER);
	if (result_size == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED)
	{
		return 0;
	}
	
	if (result_size != plane_data_size)
	{
		return 0;
	}
	
	psd->offset = psd->size;
	
	if (with_prediction)
	{
		return psd_apply_zip_prediction(psd,plane_data,plane_data_size);
	}
	
	return 1;
}

static int psd_read_compressed_image_data(_viv_psd_t *psd,BYTE *plane_data,SIZE_T plane_data_size)
{
	switch (psd->compression)
	{
		case 0:
			return psd_read_raw_image_data(psd,plane_data,plane_data_size);
			
		case 1:
			return psd_read_rle_image_data(psd,plane_data,plane_data_size);
			
		case 2:
			return psd_read_zip_image_data(psd,plane_data,plane_data_size,0);
			
		case 3:
			return psd_read_zip_image_data(psd,plane_data,plane_data_size,1);
	}
	
	return 0;
}

static int psd_get_alpha_plane_index(const _viv_psd_t *psd)
{
	if (psd->channels > psd->color_channels)
	{
		return (int)psd->color_channels;
	}
	
	return -1;
}

static BYTE psd_get_alpha_value(const _viv_psd_t *psd,const BYTE *plane_data,SIZE_T pixel_count,SIZE_T pixel_index)
{
	int alpha_plane_index;
	
	alpha_plane_index = psd_get_alpha_plane_index(psd);
	if (alpha_plane_index >= 0)
	{
		return plane_data[(pixel_count * alpha_plane_index) + pixel_index];
	}
	
	return 255;
}

static BYTE psd_clamp_byte(int value)
{
	if (value < 0)
	{
		return 0;
	}
	
	if (value > 255)
	{
		return 255;
	}
	
	return (BYTE)value;
}

static BYTE psd_clamp_double_to_byte(double value)
{
	if (value <= 0.0)
	{
		return 0;
	}
	
	if (value >= 255.0)
	{
		return 255;
	}
	
	return (BYTE)(value + 0.5);
}

static void psd_cmyk_to_rgb(int cyan,int magenta,int yellow,int black,BYTE *out_red,BYTE *out_green,BYTE *out_blue)
{
	int red;
	int green;
	int blue;
	
	red = (65535 - (cyan * (255 - black) + (black << 8))) >> 8;
	green = (65535 - (magenta * (255 - black) + (black << 8))) >> 8;
	blue = (65535 - (yellow * (255 - black) + (black << 8))) >> 8;
	
	*out_red = psd_clamp_byte(red);
	*out_green = psd_clamp_byte(green);
	*out_blue = psd_clamp_byte(blue);
}

static void psd_lab_to_rgb(int lightness,int a,int b,BYTE *out_red,BYTE *out_green,BYTE *out_blue)
{
	double x;
	double y;
	double z;
	double var_x;
	double var_y;
	double var_z;
	double var_r;
	double var_g;
	double var_b;
	
	var_y = ((double)lightness + 16.0) / 116.0;
	var_x = ((double)a / 500.0) + var_y;
	var_z = var_y - ((double)b / 200.0);
	
	if ((var_y * var_y * var_y) > 0.008856)
	{
		var_y = var_y * var_y * var_y;
	}
	else
	{
		var_y = (var_y - (16.0 / 116.0)) / 7.787;
	}
	
	if ((var_x * var_x * var_x) > 0.008856)
	{
		var_x = var_x * var_x * var_x;
	}
	else
	{
		var_x = (var_x - (16.0 / 116.0)) / 7.787;
	}
	
	if ((var_z * var_z * var_z) > 0.008856)
	{
		var_z = var_z * var_z * var_z;
	}
	else
	{
		var_z = (var_z - (16.0 / 116.0)) / 7.787;
	}
	
	x = 95.047 * var_x;
	y = 100.000 * var_y;
	z = 108.883 * var_z;
	
	var_x = x / 100.0;
	var_y = y / 100.0;
	var_z = z / 100.0;
	
	var_r = (var_x * 3.2406) + (var_y * -1.5372) + (var_z * -0.4986);
	var_g = (var_x * -0.9689) + (var_y * 1.8758) + (var_z * 0.0415);
	var_b = (var_x * 0.0557) + (var_y * -0.2040) + (var_z * 1.0570);
	
	if (var_r > 0.0031308)
	{
		var_r = (1.055 * pow(var_r,1.0 / 2.4)) - 0.055;
	}
	else
	{
		var_r *= 12.92;
	}
	
	if (var_g > 0.0031308)
	{
		var_g = (1.055 * pow(var_g,1.0 / 2.4)) - 0.055;
	}
	else
	{
		var_g *= 12.92;
	}
	
	if (var_b > 0.0031308)
	{
		var_b = (1.055 * pow(var_b,1.0 / 2.4)) - 0.055;
	}
	else
	{
		var_b *= 12.92;
	}
	
	*out_red = psd_clamp_double_to_byte(var_r * 255.0);
	*out_green = psd_clamp_double_to_byte(var_g * 255.0);
	*out_blue = psd_clamp_double_to_byte(var_b * 255.0);
}

static int psd_normalize_to_rgba8(const _viv_psd_t *psd,const BYTE *plane_data,BYTE *dst_pixels)
{
	SIZE_T pixel_count;
	SIZE_T pixel_index;
	
	if (!psd_get_pixel_count(psd,&pixel_count))
	{
		return 0;
	}
	
	for(pixel_index=0;pixel_index<pixel_count;pixel_index++)
	{
		BYTE red;
		BYTE green;
		BYTE blue;
		BYTE alpha;
		
		red = 0;
		green = 0;
		blue = 0;
		alpha = psd_get_alpha_value(psd,plane_data,pixel_count,pixel_index);
		
		switch (psd->color_mode)
		{
			case PSD_COLOR_MODE_GRAYSCALE:
			case PSD_COLOR_MODE_DUOTONE:
				red = plane_data[pixel_index];
				green = red;
				blue = red;
				break;
				
			case PSD_COLOR_MODE_INDEXED:
				{
					BYTE index;
					
					index = plane_data[pixel_index];
					red = psd->palette[index];
					green = psd->palette[256 + index];
					blue = psd->palette[512 + index];
					
					if (psd->has_transparency_index && (index == psd->transparency_index))
					{
						alpha = 0;
					}
				}
				break;
				
			case PSD_COLOR_MODE_RGB:
				red = plane_data[pixel_index];
				green = plane_data[pixel_count + pixel_index];
				blue = plane_data[(pixel_count * 2) + pixel_index];
				break;
				
			case PSD_COLOR_MODE_CMYK:
				{
					int cyan;
					int magenta;
					int yellow;
					int black;
					
					cyan = 255 - plane_data[pixel_index];
					magenta = 255 - plane_data[pixel_count + pixel_index];
					yellow = 255 - plane_data[(pixel_count * 2) + pixel_index];
					black = 255 - plane_data[(pixel_count * 3) + pixel_index];
					psd_cmyk_to_rgb(cyan,magenta,yellow,black,&red,&green,&blue);
				}
				break;
				
			case PSD_COLOR_MODE_LAB:
				psd_lab_to_rgb((plane_data[pixel_index] * 100) / 255,plane_data[pixel_count + pixel_index] - 128,plane_data[(pixel_count * 2) + pixel_index] - 128,&red,&green,&blue);
				break;
				
			case PSD_COLOR_MODE_MULTICHANNEL:
				{
					int cyan;
					int magenta;
					int yellow;
					int black;
					
					cyan = 255 - plane_data[pixel_index];
					magenta = 255 - plane_data[pixel_count + pixel_index];
					yellow = 255 - plane_data[(pixel_count * 2) + pixel_index];
					black = 0;
					
					if (psd->color_channels >= 4)
					{
						black = 255 - plane_data[(pixel_count * 3) + pixel_index];
					}
					
					psd_cmyk_to_rgb(cyan,magenta,yellow,black,&red,&green,&blue);
				}
				break;
				
			default:
				return 0;
		}
		
		dst_pixels[(pixel_index * 4) + 0] = red;
		dst_pixels[(pixel_index * 4) + 1] = green;
		dst_pixels[(pixel_index * 4) + 2] = blue;
		dst_pixels[(pixel_index * 4) + 3] = alpha;
	}
	
	return 1;
}

int psd_load(IStream *stream,void *user_data,psd_info_callback_t info_callback,psd_frame_callback_t frame_callback)
{
	int ret;
	HGLOBAL hglobal;
	
	ret = 0;
	
	if (SUCCEEDED(GetHGlobalFromStream(stream,&hglobal)))
	{
		void *data_ptr;
		
		data_ptr = GlobalLock(hglobal);
		if (data_ptr)
		{
			_viv_psd_t psd;
			SIZE_T plane_data_size;
			SIZE_T pixel_count;
			SIZE_T pixel_buffer_size;
			BYTE *plane_data;
			BYTE *pixels;
			
			os_zero_memory(&psd,sizeof(psd));
			psd.data = data_ptr;
			psd.size = GlobalSize(hglobal);
			
			plane_data = NULL;
			pixels = NULL;
			
			if (psd_read_header(&psd))
			{
				if (psd_read_color_mode_data(&psd))
				{
					if (psd_read_image_resources(&psd))
					{
						if (psd_skip_layer_mask_info(&psd))
						{
							if (psd_read_u16_be(&psd,&psd.compression))
							{
								if (psd.compression <= 3)
								{
									if (psd_get_plane_data_size(&psd,&plane_data_size))
									{
										if (psd_get_pixel_count(&psd,&pixel_count))
										{
											pixel_buffer_size = safe_size_mul(pixel_count,4);
											if (pixel_buffer_size != SIZE_MAX)
											{
												plane_data = mem_alloc(plane_data_size);
												pixels = mem_alloc(pixel_buffer_size);
												if (plane_data && pixels)
												{
													if (psd_read_compressed_image_data(&psd,plane_data,plane_data_size))
													{
														if (psd_normalize_to_rgba8(&psd,plane_data,pixels))
														{
															if (psd.has_transparency_index)
															{
																psd.has_alpha = 1;
															}
															
															if (info_callback(user_data,1,psd.width,psd.height,psd.has_alpha))
															{
																if (frame_callback(user_data,pixels,0))
																{
																	ret = 1;
																}
															}
														}
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}
			
			if (pixels)
			{
				mem_free(pixels);
			}
			
			if (plane_data)
			{
				mem_free(plane_data);
			}
			
			GlobalUnlock(hglobal);
		}
	}
	
	return ret;
}
