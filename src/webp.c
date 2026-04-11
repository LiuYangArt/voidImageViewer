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

#include "viv.h"
#include <src/webp/decode.h>
#include <src/webp/demux.h>
#include <assert.h>

static void _webp_report_profile(const WebPData *webp_data,void *user_data,webp_profile_callback_t profile_callback)
{
	WebPDemuxer *demux;
	
	if ((!webp_data) || (!profile_callback))
	{
		return;
	}
	
	demux = WebPDemux(webp_data);
	if (demux)
	{
		uint32_t flags;
		
		flags = WebPDemuxGetI(demux,WEBP_FF_FORMAT_FLAGS);
		if (flags & ICCP_FLAG)
		{
			WebPChunkIterator chunk_iter;
			
			if (WebPDemuxGetChunk(demux,"ICCP",1,&chunk_iter))
			{
				profile_callback(user_data,chunk_iter.chunk.bytes,(DWORD)chunk_iter.chunk.size);
				WebPDemuxReleaseChunkIterator(&chunk_iter);
			}
		}
		
		WebPDemuxDelete(demux);
	}
}

int webp_load(IStream *stream,void *user_data,webp_info_callback_t info_callback,webp_profile_callback_t profile_callback,webp_frame_callback_t frame_callback)
{
	int ret;
	HGLOBAL hglobal;
	
	ret = 0;
	
//	_wassert(L"test",TEXT(__FILE__),__LINE__);
	
	if (SUCCEEDED(GetHGlobalFromStream(stream,&hglobal)))
	{
		void *data_ptr;
		
		data_ptr = GlobalLock(hglobal);
		if (data_ptr)
		{
			WebPBitstreamFeatures features;
			SIZE_T data_size;
			WebPData webp_data;
			
			data_size = GlobalSize(hglobal);	
			webp_data.bytes = data_ptr;
			webp_data.size = data_size;
			
			if (WebPGetFeatures(data_ptr,data_size,&features) == VP8_STATUS_OK)
			{
				_webp_report_profile(&webp_data,user_data,profile_callback);
				
				if (features.has_animation)
				{
					WebPAnimDecoderOptions anim_decoder_options;
					WebPAnimDecoder *anim_decoder;
					
					if (WebPAnimDecoderOptionsInit(&anim_decoder_options))
					{
						anim_decoder = WebPAnimDecoderNew(&webp_data,&anim_decoder_options);
						if (anim_decoder)
						{
							WebPAnimInfo anim_info;
							
							if (WebPAnimDecoderGetInfo(anim_decoder,&anim_info))
							{
								if (info_callback(user_data,anim_info.frame_count,anim_info.canvas_width,anim_info.canvas_height,features.has_alpha))
								{
									uint8_t *frame;
									int timestamp;
									DWORD frame_run;
									DWORD last_timestamp;
																	
									frame = NULL;
									frame_run = anim_info.frame_count;
									last_timestamp = 0;
									
									ret = 1;

									while (frame_run) 
									{
										if (WebPAnimDecoderGetNext(anim_decoder, &frame, &timestamp)) 
										{
											DWORD delay;
											
											delay = 0;
											
											if ((DWORD)timestamp > last_timestamp)
											{
												delay = (DWORD)timestamp - last_timestamp;
											}
											
											if (!frame_callback(user_data,frame,delay))
											{
												ret = 0;
												break;
											}
											
											last_timestamp = timestamp;
										}
										else
										{
											ret = 0;
											break;
										}
										
										frame_run--;
									}
								}
							}
							
							WebPAnimDecoderDelete(anim_decoder);
						}
					}
				}
				else
				{
					BYTE *pixels;
					int width;
					int height;

					pixels = WebPDecodeRGBA(data_ptr,data_size,&width,&height);
					if (pixels)
					{
						if (info_callback(user_data,1,width,height,features.has_alpha))
						{
							if (frame_callback(user_data,pixels,0))
							{
								ret = 1;
							}
						}
					
						WebPFree(pixels);
					}
				}
			}
		
			GlobalUnlock(hglobal);
		}
	}
	
	return ret;
}

void __cdecl _wassert(const wchar_t * _Message, const wchar_t *_File, unsigned _Line)
{
	debug_fatal("%S(%d) : %S",_File,_Line,_Message);
}
