//
// Copyright 2025 voidtools / David Carpenter
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
// webp layer

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*webp_info_callback_t)(void *user_data,DWORD frame_count,DWORD wide,DWORD high,int has_alpha);
typedef int (*webp_profile_callback_t)(void *user_data,const BYTE *icc_data,DWORD icc_size);
typedef int (*webp_frame_callback_t)(void *user_data,BYTE *pixels,int timestamp);

int webp_load(IStream *stream,void *user_data,webp_info_callback_t info_callback,webp_profile_callback_t profile_callback,webp_frame_callback_t frame_callback);

#ifdef __cplusplus
}
#endif
