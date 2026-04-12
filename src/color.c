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
#define _COLOR_TRANSFORM_CACHE_SIZE 4
#define _COLOR_STANDARD_SRGB_PROFILE_STATE_UNINITIALIZED 0
#define _COLOR_STANDARD_SRGB_PROFILE_STATE_LOADING 1
#define _COLOR_STANDARD_SRGB_PROFILE_STATE_READY 2
#define _COLOR_STANDARD_SRGB_PROFILE_STATE_FAILED 3

typedef struct _color_profile_lookup_key_s
{
	int is_blob;
	const BYTE *blob_data;
	DWORD blob_size;
	VIV_UINT64 blob_hash;
	const wchar_t *path;

}_color_profile_lookup_key_t;

typedef struct _color_profile_cache_key_s
{
	int is_blob;
	BYTE *blob_data;
	DWORD blob_size;
	VIV_UINT64 blob_hash;
	wchar_t path[STRING_SIZE];

}_color_profile_cache_key_t;

typedef struct _color_transform_cache_entry_s
{
	LONG ref_count;
	DWORD last_used_tick;
	DWORD source_intent;
	DWORD destination_intent;
	int destination_is_srgb;
	_color_profile_cache_key_t source_key;
	_color_profile_cache_key_t destination_key;
	HTRANSFORM transform;

}_color_transform_cache_entry_t;

#ifdef _DEBUG
typedef struct _color_perf_timer_s
{
	LARGE_INTEGER start;
	LARGE_INTEGER frequency;
	int valid;

}_color_perf_timer_t;
#endif

static CRITICAL_SECTION _color_transform_cache_cs;
static volatile LONG _color_transform_cache_state = 0;
static DWORD _color_transform_cache_tick = 0;
static _color_transform_cache_entry_t _color_transform_cache[_COLOR_TRANSFORM_CACHE_SIZE];
static volatile LONG _color_standard_srgb_profile_state = 0;
static BYTE *_color_standard_srgb_profile_data = NULL;
static DWORD _color_standard_srgb_profile_size = 0;
static VIV_UINT64 _color_standard_srgb_profile_hash = 0;
static wchar_t _color_standard_srgb_profile_path[STRING_SIZE];

#ifdef _DEBUG
static void _color_perf_timer_start(_color_perf_timer_t *timer)
{
	if (!timer)
	{
		return;
	}

	timer->valid = QueryPerformanceFrequency(&timer->frequency) && QueryPerformanceCounter(&timer->start);
}

static double _color_perf_timer_elapsed_ms(const _color_perf_timer_t *timer)
{
	LARGE_INTEGER end;

	if ((!timer) || (!timer->valid) || (!QueryPerformanceCounter(&end)) || (!timer->frequency.QuadPart))
	{
		return 0.0;
	}

	return (double)(end.QuadPart - timer->start.QuadPart) * 1000.0 / (double)timer->frequency.QuadPart;
}
#endif

static VIV_UINT64 _color_hash_blob(const BYTE *data,DWORD size)
{
	VIV_UINT64 hash;
	DWORD i;

	hash = 14695981039346656037UI64;
	for(i=0;i<size;i++)
	{
		hash ^= data[i];
		hash *= 1099511628211UI64;
	}

	return hash;
}

static void _color_profile_lookup_key_init(_color_profile_lookup_key_t *key)
{
	if (key)
	{
		os_zero_memory(key,sizeof(*key));
	}
}

static void _color_profile_lookup_key_set_blob(_color_profile_lookup_key_t *key,const color_profile_t *profile)
{
	if ((!key) || (!profile) || (!profile->data) || (!profile->size))
	{
		return;
	}

	key->is_blob = 1;
	key->blob_data = profile->data;
	key->blob_size = profile->size;
	key->blob_hash = _color_hash_blob(profile->data,profile->size);
	key->path = NULL;
}

static void _color_profile_lookup_key_set_path(_color_profile_lookup_key_t *key,const wchar_t *path)
{
	if (!key)
	{
		return;
	}

	key->is_blob = 0;
	key->blob_data = NULL;
	key->blob_size = 0;
	key->blob_hash = 0;
	key->path = path;
}

static void _color_profile_cache_key_clear(_color_profile_cache_key_t *key)
{
	if (!key)
	{
		return;
	}

	if (key->blob_data)
	{
		mem_free(key->blob_data);
	}

	os_zero_memory(key,sizeof(*key));
}

static int _color_compare_blob_data(const BYTE *a,const BYTE *b,DWORD size)
{
	DWORD i;

	if ((!a) || (!b))
	{
		return 0;
	}

	for(i=0;i<size;i++)
	{
		if (a[i] != b[i])
		{
			return 0;
		}
	}

	return 1;
}

static int _color_profile_cache_key_copy(_color_profile_cache_key_t *dst,const _color_profile_lookup_key_t *src)
{
	if ((!dst) || (!src))
	{
		return 0;
	}

	_color_profile_cache_key_clear(dst);
	dst->is_blob = src->is_blob;
	dst->blob_size = src->blob_size;
	dst->blob_hash = src->blob_hash;

	if (src->is_blob)
	{
		if ((!src->blob_data) || (!src->blob_size))
		{
			return 0;
		}

		dst->blob_data = (BYTE *)mem_alloc(src->blob_size);
		if (!dst->blob_data)
		{
			_color_profile_cache_key_clear(dst);
			return 0;
		}

		os_copy_memory(dst->blob_data,src->blob_data,(int)src->blob_size);
	}
	else
	{
		if ((src->path) && (*src->path))
		{
			string_copy(dst->path,src->path);
		}
	}

	return 1;
}

static int _color_profile_cache_key_matches(const _color_profile_cache_key_t *cache_key,const _color_profile_lookup_key_t *lookup_key)
{
	if ((!cache_key) || (!lookup_key) || (cache_key->is_blob != lookup_key->is_blob))
	{
		return 0;
	}

	if (cache_key->is_blob)
	{
		return (cache_key->blob_size == lookup_key->blob_size) &&
			(cache_key->blob_hash == lookup_key->blob_hash) &&
			(cache_key->blob_data) &&
			(lookup_key->blob_data) &&
			(_color_compare_blob_data(cache_key->blob_data,lookup_key->blob_data,cache_key->blob_size));
	}

	if ((!cache_key->path[0]) && ((!lookup_key->path) || (!*lookup_key->path)))
	{
		return 1;
	}

	if ((!lookup_key->path) || (!*lookup_key->path))
	{
		return 0;
	}

	return string_compare(cache_key->path,lookup_key->path) == 0;
}

static void _color_transform_cache_entry_clear(_color_transform_cache_entry_t *entry)
{
	if (!entry)
	{
		return;
	}

	if (entry->transform)
	{
		os_DeleteColorTransform(entry->transform);
	}

	_color_profile_cache_key_clear(&entry->source_key);
	_color_profile_cache_key_clear(&entry->destination_key);
	os_zero_memory(entry,sizeof(*entry));
}

static void _color_transform_cache_init(void)
{
	LONG state;

	state = _color_transform_cache_state;
	if (state == 2)
	{
		return;
	}

	if (InterlockedCompareExchange((LONG *)&_color_transform_cache_state,1,0) == 0)
	{
		InitializeCriticalSection(&_color_transform_cache_cs);
		os_zero_memory(_color_transform_cache,sizeof(_color_transform_cache));
		_color_transform_cache_tick = 0;
		_color_transform_cache_state = 2;
		return;
	}

	while(_color_transform_cache_state != 2)
	{
		Sleep(0);
	}
}

static void _color_transform_cache_lock(void)
{
	_color_transform_cache_init();
	EnterCriticalSection(&_color_transform_cache_cs);
}

static void _color_transform_cache_unlock(void)
{
	LeaveCriticalSection(&_color_transform_cache_cs);
}

static _color_transform_cache_entry_t *_color_transform_cache_find_locked(const _color_profile_lookup_key_t *source_key,const _color_profile_lookup_key_t *destination_key,DWORD source_intent,DWORD destination_intent,int destination_is_srgb)
{
	int i;

	for(i=0;i<_COLOR_TRANSFORM_CACHE_SIZE;i++)
	{
		if ((_color_transform_cache[i].transform) &&
			(_color_transform_cache[i].source_intent == source_intent) &&
			(_color_transform_cache[i].destination_intent == destination_intent) &&
			(_color_transform_cache[i].destination_is_srgb == destination_is_srgb) &&
			(_color_profile_cache_key_matches(&_color_transform_cache[i].source_key,source_key)) &&
			(_color_profile_cache_key_matches(&_color_transform_cache[i].destination_key,destination_key)))
		{
			return &_color_transform_cache[i];
		}
	}

	return NULL;
}

static _color_transform_cache_entry_t *_color_transform_cache_acquire(const _color_profile_lookup_key_t *source_key,const _color_profile_lookup_key_t *destination_key,DWORD source_intent,DWORD destination_intent,int destination_is_srgb)
{
	_color_transform_cache_entry_t *entry;

	entry = NULL;
	_color_transform_cache_lock();
	entry = _color_transform_cache_find_locked(source_key,destination_key,source_intent,destination_intent,destination_is_srgb);
	if (entry)
	{
		entry->ref_count++;
		entry->last_used_tick = ++_color_transform_cache_tick;
	}
	_color_transform_cache_unlock();

	return entry;
}

static void _color_transform_cache_release(_color_transform_cache_entry_t *entry)
{
	if ((!entry) || (_color_transform_cache_state != 2))
	{
		return;
	}

	_color_transform_cache_lock();
	if (entry->ref_count > 0)
	{
		entry->ref_count--;
	}
	_color_transform_cache_unlock();
}

static int _color_transform_cache_find_victim_locked(void)
{
	DWORD oldest_tick;
	int i;
	int oldest_index;

	oldest_tick = 0;
	oldest_index = -1;
	for(i=0;i<_COLOR_TRANSFORM_CACHE_SIZE;i++)
	{
		if ((!_color_transform_cache[i].transform) && (_color_transform_cache[i].ref_count == 0))
		{
			return i;
		}

		if ((_color_transform_cache[i].ref_count == 0) && ((oldest_index == -1) || (_color_transform_cache[i].last_used_tick < oldest_tick)))
		{
			oldest_tick = _color_transform_cache[i].last_used_tick;
			oldest_index = i;
		}
	}

	return oldest_index;
}

static _color_transform_cache_entry_t *_color_transform_cache_store(const _color_profile_lookup_key_t *source_key,const _color_profile_lookup_key_t *destination_key,DWORD source_intent,DWORD destination_intent,int destination_is_srgb,HTRANSFORM transform,int *stored_local_transform)
{
	_color_transform_cache_entry_t *entry;
	int victim_index;

	if (stored_local_transform)
	{
		*stored_local_transform = 0;
	}

	if (!transform)
	{
		return NULL;
	}

	entry = NULL;
	_color_transform_cache_lock();
	entry = _color_transform_cache_find_locked(source_key,destination_key,source_intent,destination_intent,destination_is_srgb);
	if (entry)
	{
		entry->ref_count++;
		entry->last_used_tick = ++_color_transform_cache_tick;
		_color_transform_cache_unlock();
		return entry;
	}

	victim_index = _color_transform_cache_find_victim_locked();
	if (victim_index == -1)
	{
		_color_transform_cache_unlock();
		return NULL;
	}

	entry = &_color_transform_cache[victim_index];
	_color_transform_cache_entry_clear(entry);
	if ((!_color_profile_cache_key_copy(&entry->source_key,source_key)) || (!_color_profile_cache_key_copy(&entry->destination_key,destination_key)))
	{
		_color_transform_cache_entry_clear(entry);
		_color_transform_cache_unlock();
		return NULL;
	}

	entry->source_intent = source_intent;
	entry->destination_intent = destination_intent;
	entry->destination_is_srgb = destination_is_srgb;
	entry->transform = transform;
	entry->ref_count = 1;
	entry->last_used_tick = ++_color_transform_cache_tick;
	if (stored_local_transform)
	{
		*stored_local_transform = 1;
	}
	_color_transform_cache_unlock();

	return entry;
}

static void _color_debug_log_transform(const char *label,DWORD wide,DWORD high,const _color_profile_lookup_key_t *source_key,const _color_profile_lookup_key_t *destination_key,int cache_hit,double create_ms,double translate_ms,double total_ms,int fallback_copy)
{
#ifdef _DEBUG
	const wchar_t *source_path;
	const wchar_t *destination_path;

	source_path = ((source_key) && (source_key->path)) ? source_key->path : L"";
	destination_path = ((destination_key) && (destination_key->path)) ? destination_key->path : L"";
	debug_printf("ICC %s %u x %u cache %s src_blob %d src_hash 0x%I64x src_path %S dst_path %S create %.3fms translate %.3fms total %.3fms fallback %d\n",label,wide,high,cache_hit ? "hit" : "miss",(source_key) ? source_key->is_blob : 0,(source_key) ? source_key->blob_hash : 0,source_path,destination_path,create_ms,translate_ms,total_ms,fallback_copy);
#else
	(void)label;
	(void)wide;
	(void)high;
	(void)source_key;
	(void)destination_key;
	(void)cache_hit;
	(void)create_ms;
	(void)translate_ms;
	(void)total_ms;
	(void)fallback_copy;
#endif
}

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

static int _color_read_profile_file(const wchar_t *path,BYTE **out_data,DWORD *out_size)
{
	HANDLE file;
	LARGE_INTEGER size;
	BYTE *data;
	DWORD bytes_read;

	if ((!path) || (!*path) || (!out_data) || (!out_size))
	{
		return 0;
	}

	file = CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
	if (file == INVALID_HANDLE_VALUE)
	{
		return 0;
	}

	if ((!GetFileSizeEx(file,&size)) || (size.QuadPart <= 0) || (size.QuadPart > _COLOR_MAX_PROFILE_SIZE) || (size.QuadPart > 0xffffffffUI64))
	{
		CloseHandle(file);
		return 0;
	}

	data = (BYTE *)mem_alloc((SIZE_T)size.QuadPart);
	if (!data)
	{
		CloseHandle(file);
		return 0;
	}

	bytes_read = 0;
	if ((!ReadFile(file,data,(DWORD)size.QuadPart,&bytes_read,NULL)) || (bytes_read != (DWORD)size.QuadPart))
	{
		mem_free(data);
		CloseHandle(file);
		return 0;
	}

	*out_data = data;
	*out_size = (DWORD)size.QuadPart;
	CloseHandle(file);
	return 1;
}

static int _color_ensure_standard_srgb_profile_loaded(void)
{
	LONG state;
	BYTE *data;
	DWORD size;
	wchar_t path[STRING_SIZE];

	state = _color_standard_srgb_profile_state;
	if (state == _COLOR_STANDARD_SRGB_PROFILE_STATE_READY)
	{
		return 1;
	}

	if (state == _COLOR_STANDARD_SRGB_PROFILE_STATE_FAILED)
	{
		return 0;
	}

	if (InterlockedCompareExchange(&_color_standard_srgb_profile_state,_COLOR_STANDARD_SRGB_PROFILE_STATE_LOADING,_COLOR_STANDARD_SRGB_PROFILE_STATE_UNINITIALIZED) != 0)
	{
		while(_color_standard_srgb_profile_state == _COLOR_STANDARD_SRGB_PROFILE_STATE_LOADING)
		{
			Sleep(0);
		}

		return _color_standard_srgb_profile_state == _COLOR_STANDARD_SRGB_PROFILE_STATE_READY;
	}

	path[0] = 0;
	data = NULL;
	size = 0;
	if ((_color_get_profile_path_for_srgb(path,STRING_SIZE)) && (_color_read_profile_file(path,&data,&size)))
	{
		_color_standard_srgb_profile_data = data;
		_color_standard_srgb_profile_size = size;
		_color_standard_srgb_profile_hash = _color_hash_blob(data,size);
		string_copy(_color_standard_srgb_profile_path,path);
		InterlockedExchange(&_color_standard_srgb_profile_state,_COLOR_STANDARD_SRGB_PROFILE_STATE_READY);
		return 1;
	}

	InterlockedExchange(&_color_standard_srgb_profile_state,_COLOR_STANDARD_SRGB_PROFILE_STATE_FAILED);
	return 0;
}

static int _color_lookup_key_is_standard_srgb(const _color_profile_lookup_key_t *key)
{
	if (!key)
	{
		return 0;
	}

	if (!_color_ensure_standard_srgb_profile_loaded())
	{
		return 0;
	}

	if (key->is_blob)
	{
		return (key->blob_size == _color_standard_srgb_profile_size) && (key->blob_hash == _color_standard_srgb_profile_hash) && (_color_compare_blob_data(key->blob_data,_color_standard_srgb_profile_data,key->blob_size));
	}

	if ((key->path) && (*key->path))
	{
		return string_compare(key->path,_color_standard_srgb_profile_path) == 0;
	}

	return 0;
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

static HPROFILE _color_open_profile_from_path(const wchar_t *profile_path);

static HPROFILE _color_open_profile_from_lookup_key(const _color_profile_lookup_key_t *key)
{
	color_profile_t profile;

	if (!key)
	{
		return NULL;
	}

	if (key->is_blob)
	{
		profile.data = (BYTE *)key->blob_data;
		profile.size = key->blob_size;
		return _color_open_profile_from_blob(&profile);
	}

	return _color_open_profile_from_path(key->path);
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
	HTRANSFORM local_transform;
	HPROFILE source_hprofile;
	HPROFILE destination_hprofile;
	wchar_t srgb_profile_path[STRING_SIZE];
	const wchar_t *resolved_source_profile_path;
	const wchar_t *resolved_destination_profile_path;
	_color_profile_lookup_key_t source_lookup_key;
	_color_profile_lookup_key_t destination_lookup_key;
	_color_transform_cache_entry_t *cache_entry;
	DWORD intents[2];
	HPROFILE profiles[2];
	SIZE_T bitmap_size;
	BYTE *temp_pixels;
	DWORD source_intent;
	DWORD destination_intent;
	int cache_hit;
	int fallback_copy;
	int stored_local_transform;
	int ret;
	#ifdef _DEBUG
	_color_perf_timer_t total_timer;
	_color_perf_timer_t create_timer;
	_color_perf_timer_t translate_timer;
	double create_ms;
	double translate_ms;
	double total_ms;
	#endif
	
	ret = 0;
	transform = NULL;
	local_transform = NULL;
	source_hprofile = NULL;
	destination_hprofile = NULL;
	resolved_source_profile_path = source_profile_path;
	resolved_destination_profile_path = destination_profile_path;
	_color_profile_lookup_key_init(&source_lookup_key);
	_color_profile_lookup_key_init(&destination_lookup_key);
	cache_entry = NULL;
	temp_pixels = NULL;
	srgb_profile_path[0] = 0;
	source_intent = INTENT_PERCEPTUAL;
	destination_intent = INTENT_PERCEPTUAL;
	cache_hit = 0;
	fallback_copy = 0;
	stored_local_transform = 0;
	#ifdef _DEBUG
	create_ms = 0.0;
	translate_ms = 0.0;
	total_ms = 0.0;
	_color_perf_timer_start(&total_timer);
	#endif
	
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

	if ((source_profile) && (source_profile->data) && (source_profile->size))
	{
		_color_profile_lookup_key_set_blob(&source_lookup_key,source_profile);
	}
	else
	{
		_color_profile_lookup_key_set_path(&source_lookup_key,resolved_source_profile_path);
	}

	_color_profile_lookup_key_set_path(&destination_lookup_key,resolved_destination_profile_path);
	if ((_color_lookup_key_is_standard_srgb(&source_lookup_key)) && ((_color_lookup_key_is_standard_srgb(&destination_lookup_key)) || (destination_is_srgb)))
	{
		return _color_copy_pixels(src_pixels,dst_pixels,wide,high);
	}

	cache_entry = _color_transform_cache_acquire(&source_lookup_key,&destination_lookup_key,source_intent,destination_intent,destination_is_srgb);
	if (cache_entry)
	{
		cache_hit = 1;
		transform = cache_entry->transform;
	}

	if (!transform)
	{
		#ifdef _DEBUG
		_color_perf_timer_start(&create_timer);
		#endif
		source_hprofile = _color_open_profile_from_lookup_key(&source_lookup_key);
		destination_hprofile = _color_open_profile_from_lookup_key(&destination_lookup_key);
		
		if ((!source_hprofile) || (!destination_hprofile) || (!os_CreateMultiProfileTransform) || (!os_TranslateBitmapBits))
		{
			fallback_copy = 1;
			ret = _color_copy_pixels(src_pixels,dst_pixels,wide,high);
			goto end;
		}
		
		profiles[0] = source_hprofile;
		profiles[1] = destination_hprofile;
		intents[0] = source_intent;
		intents[1] = destination_intent;
		
		local_transform = os_CreateMultiProfileTransform(profiles,2,intents,2,0,INDEX_DONT_CARE);
		#ifdef _DEBUG
		create_ms = _color_perf_timer_elapsed_ms(&create_timer);
		#endif
		if (!local_transform)
		{
			fallback_copy = 1;
			ret = _color_copy_pixels(src_pixels,dst_pixels,wide,high);
			goto end;
		}

		cache_entry = _color_transform_cache_store(&source_lookup_key,&destination_lookup_key,source_intent,destination_intent,destination_is_srgb,local_transform,&stored_local_transform);
		if (cache_entry)
		{
			transform = cache_entry->transform;
			if (stored_local_transform)
			{
				local_transform = NULL;
			}
		}
		else
		{
			transform = local_transform;
		}
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
	
	#ifdef _DEBUG
	_color_perf_timer_start(&translate_timer);
	#endif
	if (os_TranslateBitmapBits(transform,(void *)src_pixels,BM_xRGBQUADS,wide,high,wide * 4,dst_pixels,BM_xRGBQUADS,wide * 4,NULL,0))
	{
		#ifdef _DEBUG
		translate_ms = _color_perf_timer_elapsed_ms(&translate_timer);
		#endif
		if (temp_pixels)
		{
			os_copy_memory((BYTE *)src_pixels,temp_pixels,(int)bitmap_size);
		}
		
		ret = 1;
	}
	else
	{
		#ifdef _DEBUG
		translate_ms = _color_perf_timer_elapsed_ms(&translate_timer);
		#endif
		fallback_copy = 1;
		ret = _color_copy_pixels(src_pixels,(temp_pixels ? (BYTE *)src_pixels : dst_pixels),wide,high);
	}
	
end:
	#ifdef _DEBUG
	total_ms = _color_perf_timer_elapsed_ms(&total_timer);
	_color_debug_log_transform(destination_is_srgb ? "source-to-srgb" : "srgb-to-display",wide,high,&source_lookup_key,&destination_lookup_key,cache_hit,create_ms,translate_ms,total_ms,fallback_copy);
	#endif
	if (cache_entry)
	{
		_color_transform_cache_release(cache_entry);
	}

	if (local_transform)
	{
		os_DeleteColorTransform(local_transform);
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

void color_clear_transform_cache(void)
{
	int i;

	if (_color_transform_cache_state != 2)
	{
		return;
	}

	_color_transform_cache_lock();
	for(i=0;i<_COLOR_TRANSFORM_CACHE_SIZE;i++)
	{
		_color_transform_cache_entry_clear(&_color_transform_cache[i]);
	}
	_color_transform_cache_unlock();
	DeleteCriticalSection(&_color_transform_cache_cs);
	_color_transform_cache_state = 0;

	if (_color_standard_srgb_profile_data)
	{
		mem_free(_color_standard_srgb_profile_data);
		_color_standard_srgb_profile_data = NULL;
	}

	_color_standard_srgb_profile_size = 0;
	_color_standard_srgb_profile_hash = 0;
	_color_standard_srgb_profile_path[0] = 0;
	_color_standard_srgb_profile_state = _COLOR_STANDARD_SRGB_PROFILE_STATE_UNINITIALIZED;
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
	void *dib_pixels;
	SIZE_T bitmap_size;
	#ifdef _DEBUG
	_color_perf_timer_t timer;
	#endif
	
	if ((!wide) || (!high) || (!pixels))
	{
		return NULL;
	}

	if (!color_get_bgra_size(wide,high,&bitmap_size))
	{
		return NULL;
	}

	if (bitmap_size > 0x7fffffffU)
	{
		return NULL;
	}

	#ifdef _DEBUG
	_color_perf_timer_start(&timer);
	#endif
	
	os_zero_memory(&bmi,sizeof(BITMAPINFO));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = wide;
	bmi.bmiHeader.biHeight = -(int)high;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;
	
	hbitmap = NULL;
	dib_pixels = NULL;
	screen_hdc = GetDC(NULL);
	if (screen_hdc)
	{
		hbitmap = CreateDIBSection(screen_hdc,&bmi,DIB_RGB_COLORS,&dib_pixels,NULL,0);
		if ((hbitmap) && (dib_pixels))
		{
			os_copy_memory(dib_pixels,pixels,(int)bitmap_size);
		}
		else if (hbitmap)
		{
			DeleteObject(hbitmap);
			hbitmap = NULL;
		}
		
		ReleaseDC(NULL,screen_hdc);
	}

	#ifdef _DEBUG
	debug_printf("ICC create-hbitmap %u x %u total %.3fms success %d\n",wide,high,_color_perf_timer_elapsed_ms(&timer),hbitmap ? 1 : 0);
	#endif
	
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
