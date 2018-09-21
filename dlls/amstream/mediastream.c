/*
 * Implementation of IMediaStream Interfaces
 *
 * Copyright 2005, 2008, 2012 Christian Costa
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define NONAMELESSUNION
#define NONAMELESSSTRUCT
#define COBJMACROS

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "dshow.h"

#include "wine/strmbase.h"
#include "wine/list.h"

#include "amstream_private.h"

#include "ddstream.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(amstream);

static HRESULT ddrawstreamsample_create(IDirectDrawMediaStream *parent, IDirectDrawSurface *surface,
    const RECT *rect, IDirectDrawStreamSample **ddraw_stream_sample);
static HRESULT audiostreamsample_create(IAudioMediaStream *parent, IAudioData *audio_data, IAudioStreamSample **audio_stream_sample);

struct DirectDrawMediaStreamImpl;

typedef struct {
    BaseInputPin pin;
    struct DirectDrawMediaStreamImpl *parent;
} DirectDrawMediaStreamInputPin;

typedef struct DirectDrawMediaStreamImpl {
    IAMMediaStream IAMMediaStream_iface;
    IDirectDrawMediaStream IDirectDrawMediaStream_iface;
    LONG ref;
    IMultiMediaStream* parent;
    MSPID purpose_id;
    STREAM_TYPE stream_type;
    IDirectDraw7 *ddraw;
    DirectDrawMediaStreamInputPin *input_pin;
    IFilterGraph *graph;
    CRITICAL_SECTION critical_section;
    DDSURFACEDESC format;
    FILTER_STATE state;
    struct list sample_queue;
    HANDLE sample_queued_event;
} DirectDrawMediaStreamImpl;

static inline DirectDrawMediaStreamImpl *impl_from_DirectDrawMediaStream_IAMMediaStream(IAMMediaStream *iface)
{
    return CONTAINING_RECORD(iface, DirectDrawMediaStreamImpl, IAMMediaStream_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_QueryInterface(IAMMediaStream *iface,
                                                        REFIID riid, void **ret_iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%s,%p)\n", iface, This, debugstr_guid(riid), ret_iface);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IMediaStream) ||
        IsEqualGUID(riid, &IID_IAMMediaStream))
    {
        IAMMediaStream_AddRef(iface);
        *ret_iface = iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IDirectDrawMediaStream))
    {
        IAMMediaStream_AddRef(iface);
        *ret_iface = &This->IDirectDrawMediaStream_iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IPin))
    {
        IAMMediaStream_AddRef(iface);
        *ret_iface = &This->input_pin->pin.pin.IPin_iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IMemInputPin))
    {
        IAMMediaStream_AddRef(iface);
        *ret_iface = &This->input_pin->pin.IMemInputPin_iface;
        return S_OK;
    }

    ERR("(%p)->(%s,%p),not found\n", This, debugstr_guid(riid), ret_iface);
    return E_NOINTERFACE;
}

static ULONG WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_AddRef(IAMMediaStream *iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p/%p)->(): new ref = %u\n", iface, This, ref);

    return ref;
}

static ULONG WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_Release(IAMMediaStream *iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p/%p)->(): new ref = %u\n", iface, This, ref);

    if (!ref)
    {
        BaseInputPin_Destroy((BaseInputPin *)This->input_pin);
        DeleteCriticalSection(&This->critical_section);
        if (This->ddraw)
            IDirectDraw7_Release(This->ddraw);
        HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

/*** IMediaStream methods ***/
static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_GetMultiMediaStream(IAMMediaStream *iface,
        IMultiMediaStream** multi_media_stream)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", This, iface, multi_media_stream);

    if (!multi_media_stream)
        return E_POINTER;

    IMultiMediaStream_AddRef(This->parent);
    *multi_media_stream = This->parent;

    return S_OK;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_GetInformation(IAMMediaStream *iface,
        MSPID *purpose_id, STREAM_TYPE *type)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%p,%p)\n", This, iface, purpose_id, type);

    if (purpose_id)
        *purpose_id = This->purpose_id;
    if (type)
        *type = This->stream_type;

    return S_OK;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_SetSameFormat(IAMMediaStream *iface,
        IMediaStream *pStreamThatHasDesiredFormat, DWORD flags)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%p,%x) stub!\n", This, iface, pStreamThatHasDesiredFormat, flags);

    return S_FALSE;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_AllocateSample(IAMMediaStream *iface,
        DWORD flags, IStreamSample **sample)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%x,%p) stub!\n", This, iface, flags, sample);

    return S_FALSE;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_CreateSharedSample(IAMMediaStream *iface,
        IStreamSample *existing_sample, DWORD flags, IStreamSample **sample)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%p,%x,%p) stub!\n", This, iface, existing_sample, flags, sample);

    return S_FALSE;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_SendEndOfStream(IAMMediaStream *iface, DWORD flags)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%x) stub!\n", This, iface, flags);

    return S_FALSE;
}

/*** IAMMediaStream methods ***/
static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_Initialize(IAMMediaStream *iface, IUnknown *source_object, DWORD flags,
                                                    REFMSPID purpose_id, const STREAM_TYPE stream_type)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%p,%x,%p,%u) stub!\n", This, iface, source_object, flags, purpose_id, stream_type);

    return S_FALSE;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_SetState(IAMMediaStream *iface, FILTER_STATE state)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);
    HRESULT hr = S_OK;

    TRACE("(%p/%p)->(%u)\n", This, iface, state);

    EnterCriticalSection(&This->critical_section);

    if (This->state == state)
        goto out_critical_section;

    switch (state)
    {
    case State_Stopped:
        SetEvent(This->sample_queued_event);
        break;
    case State_Paused:
    case State_Running:
        if (State_Stopped == This->state)
            This->input_pin->pin.end_of_stream = FALSE;
        break;
    default:
        hr = E_INVALIDARG;
        goto out_critical_section;
    }

    This->state = state;

out_critical_section:
    LeaveCriticalSection(&This->critical_section);

    return hr;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_JoinAMMultiMediaStream(IAMMediaStream *iface, IAMMultiMediaStream *am_multi_media_stream)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%p) stub!\n", This, iface, am_multi_media_stream);

    return S_FALSE;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_JoinFilter(IAMMediaStream *iface, IMediaStreamFilter *media_stream_filter)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", This, iface, media_stream_filter);

    This->input_pin->pin.pin.pinInfo.pFilter = (IBaseFilter *)media_stream_filter;

    return S_OK;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_JoinFilterGraph(IAMMediaStream *iface, IFilterGraph *filtergraph)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", This, iface, filtergraph);

    This->graph = filtergraph;

    return S_OK;
}

static const struct IAMMediaStreamVtbl DirectDrawMediaStreamImpl_IAMMediaStream_Vtbl =
{
    /*** IUnknown methods ***/
    DirectDrawMediaStreamImpl_IAMMediaStream_QueryInterface,
    DirectDrawMediaStreamImpl_IAMMediaStream_AddRef,
    DirectDrawMediaStreamImpl_IAMMediaStream_Release,
    /*** IMediaStream methods ***/
    DirectDrawMediaStreamImpl_IAMMediaStream_GetMultiMediaStream,
    DirectDrawMediaStreamImpl_IAMMediaStream_GetInformation,
    DirectDrawMediaStreamImpl_IAMMediaStream_SetSameFormat,
    DirectDrawMediaStreamImpl_IAMMediaStream_AllocateSample,
    DirectDrawMediaStreamImpl_IAMMediaStream_CreateSharedSample,
    DirectDrawMediaStreamImpl_IAMMediaStream_SendEndOfStream,
    /*** IAMMediaStream methods ***/
    DirectDrawMediaStreamImpl_IAMMediaStream_Initialize,
    DirectDrawMediaStreamImpl_IAMMediaStream_SetState,
    DirectDrawMediaStreamImpl_IAMMediaStream_JoinAMMultiMediaStream,
    DirectDrawMediaStreamImpl_IAMMediaStream_JoinFilter,
    DirectDrawMediaStreamImpl_IAMMediaStream_JoinFilterGraph
};

static const GUID *ddrawmediastream_subtype_from_format(const DDPIXELFORMAT *format)
{
    switch (format->u1.dwRGBBitCount)
    {
    case 8:
        return &MEDIASUBTYPE_RGB8;
    case 16:
        return format->u3.dwGBitMask == 0x7e0 ? &MEDIASUBTYPE_RGB565 : &MEDIASUBTYPE_RGB555;
    case 24:
        return &MEDIASUBTYPE_RGB24;
    case 32:
        return &MEDIASUBTYPE_RGB32;
    }
    return &GUID_NULL;
}

static HRESULT ddrawmediastream_is_media_type_compatible(const AM_MEDIA_TYPE *media_type, const DDSURFACEDESC *format)
{
    const VIDEOINFOHEADER *video_info;

    if (!IsEqualGUID(&media_type->majortype, &MEDIATYPE_Video))
        return S_FALSE;

    if (!IsEqualGUID(&media_type->formattype, &FORMAT_VideoInfo))
        return S_FALSE;

    if (!media_type->pbFormat)
        return S_FALSE;

    if (media_type->cbFormat < sizeof(VIDEOINFOHEADER))
        return S_FALSE;

    video_info = (const VIDEOINFOHEADER *)media_type->pbFormat;

    if (format->dwFlags & DDSD_HEIGHT)
    {
        if (video_info->bmiHeader.biWidth != format->dwWidth)
            return S_FALSE;
        if (abs(video_info->bmiHeader.biHeight) != format->dwHeight)
            return S_FALSE;
    }

    if (format->dwFlags & DDSD_PIXELFORMAT)
    {
        const GUID *subtype = ddrawmediastream_subtype_from_format(&format->ddpfPixelFormat);
        if (!IsEqualGUID(&media_type->subtype, subtype))
            return S_FALSE;
    }
    else
    {
        if (!IsEqualGUID(&media_type->subtype, &MEDIASUBTYPE_RGB8)  &&
            !IsEqualGUID(&media_type->subtype, &MEDIASUBTYPE_RGB565) &&
            !IsEqualGUID(&media_type->subtype, &MEDIASUBTYPE_RGB555) &&
            !IsEqualGUID(&media_type->subtype, &MEDIASUBTYPE_RGB24) &&
            !IsEqualGUID(&media_type->subtype, &MEDIASUBTYPE_RGB32))
            return S_FALSE;
    }

    return S_OK;
}

static HRESULT ddrawmediastream_reconnect(DirectDrawMediaStreamImpl *This, const DDSURFACEDESC *format)
{
    IGraphBuilder *graph_builder;
    DDSURFACEDESC old_format;
    AM_MEDIA_TYPE old_media_type;
    IPin *connected_to;
    HRESULT hr;

    hr = IFilterGraph_QueryInterface(This->graph, &IID_IGraphBuilder, (void **)&graph_builder);
    if (FAILED(hr))
        return hr;

    old_format = This->format;
    hr = CopyMediaType(&old_media_type, &This->input_pin->pin.pin.mtCurrent);
    if (FAILED(hr))
        goto out_graph_builder;
    connected_to = This->input_pin->pin.pin.pConnectedTo;
    IPin_AddRef(connected_to);

    IGraphBuilder_Disconnect(graph_builder, connected_to);
    IGraphBuilder_Disconnect(graph_builder, &This->input_pin->pin.pin.IPin_iface);
    This->format = *format;
    hr = IGraphBuilder_Connect(graph_builder, connected_to, &This->input_pin->pin.pin.IPin_iface);
    if (FAILED(hr))
    {
        This->format = old_format;
        IGraphBuilder_ConnectDirect(graph_builder, connected_to, &This->input_pin->pin.pin.IPin_iface, &old_media_type);
    }

    IPin_Release(connected_to);
    FreeMediaType(&old_media_type);

out_graph_builder:
    IGraphBuilder_Release(graph_builder);

    return hr;
}

static HRESULT ddrawmediastream_is_format_valid(const DDSURFACEDESC *format)
{
    if (format->dwFlags & DDSD_PIXELFORMAT)
    {
        if (format->ddpfPixelFormat.dwFlags & (DDPF_YUV | DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 | DDPF_PALETTEINDEXED4 | DDPF_PALETTEINDEXEDTO8))
            return S_FALSE;

        if (!(format->ddpfPixelFormat.dwFlags & DDPF_RGB))
            return S_FALSE;

        switch (format->ddpfPixelFormat.u1.dwRGBBitCount)
        {
        case 8:
            if (!(format->ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8))
                return S_FALSE;
            break;
        case 16:
            if (format->ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8)
                return S_FALSE;
            if ((format->ddpfPixelFormat.u2.dwRBitMask != 0x7c00 ||
                format->ddpfPixelFormat.u3.dwGBitMask != 0x03e0 ||
                format->ddpfPixelFormat.u4.dwBBitMask != 0x001f) &&
                (format->ddpfPixelFormat.u2.dwRBitMask != 0xf800 ||
                format->ddpfPixelFormat.u3.dwGBitMask != 0x07e0 ||
                format->ddpfPixelFormat.u4.dwBBitMask != 0x001f))
                return S_FALSE;
            break;
        case 24:
        case 32:
            if (format->ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8)
                return S_FALSE;
            if (format->ddpfPixelFormat.u2.dwRBitMask != 0xff0000 ||
                format->ddpfPixelFormat.u3.dwGBitMask != 0x00ff00 ||
                format->ddpfPixelFormat.u4.dwBBitMask != 0x0000ff)
                return S_FALSE;
            break;
        default:
            return S_FALSE;
        }
    }
    return S_OK;
}

static inline DirectDrawMediaStreamImpl *impl_from_IDirectDrawMediaStream(IDirectDrawMediaStream *iface)
{
    return CONTAINING_RECORD(iface, DirectDrawMediaStreamImpl, IDirectDrawMediaStream_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_QueryInterface(IDirectDrawMediaStream *iface,
        REFIID riid, void **ret_iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);
    TRACE("(%p/%p)->(%s,%p)\n", iface, This, debugstr_guid(riid), ret_iface);
    return IAMMediaStream_QueryInterface(&This->IAMMediaStream_iface, riid, ret_iface);
}

static ULONG WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_AddRef(IDirectDrawMediaStream *iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);
    TRACE("(%p/%p)\n", iface, This);
    return IAMMediaStream_AddRef(&This->IAMMediaStream_iface);
}

static ULONG WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_Release(IDirectDrawMediaStream *iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);
    TRACE("(%p/%p)\n", iface, This);
    return IAMMediaStream_Release(&This->IAMMediaStream_iface);
}

/*** IMediaStream methods ***/
static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetMultiMediaStream(IDirectDrawMediaStream *iface,
        IMultiMediaStream **multi_media_stream)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", This, iface, multi_media_stream);

    if (!multi_media_stream)
        return E_POINTER;

    IMultiMediaStream_AddRef(This->parent);
    *multi_media_stream = This->parent;

    return S_OK;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetInformation(IDirectDrawMediaStream *iface,
        MSPID *purpose_id, STREAM_TYPE *type)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);

    TRACE("(%p/%p)->(%p,%p)\n", This, iface, purpose_id, type);

    if (purpose_id)
        *purpose_id = This->purpose_id;
    if (type)
        *type = This->stream_type;

    return S_OK;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_SetSameFormat(IDirectDrawMediaStream *iface,
        IMediaStream *pStreamThatHasDesiredFormat, DWORD dwFlags)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);

    FIXME("(%p/%p)->(%p,%x) stub!\n", This, iface, pStreamThatHasDesiredFormat, dwFlags);

    return S_FALSE;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_AllocateSample(IDirectDrawMediaStream *iface,
        DWORD dwFlags, IStreamSample **ppSample)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);

    FIXME("(%p/%p)->(%x,%p) stub!\n", This, iface, dwFlags, ppSample);

    return S_FALSE;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_CreateSharedSample(IDirectDrawMediaStream *iface,
        IStreamSample *pExistingSample, DWORD dwFlags, IStreamSample **ppSample)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);

    FIXME("(%p/%p)->(%p,%x,%p) stub!\n", This, iface, pExistingSample, dwFlags, ppSample);

    return S_FALSE;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_SendEndOfStream(IDirectDrawMediaStream *iface,
        DWORD dwFlags)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);

    FIXME("(%p/%p)->(%x) stub!\n", This, iface, dwFlags);

    return S_FALSE;
}

/*** IDirectDrawMediaStream methods ***/
static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetFormat(IDirectDrawMediaStream *iface,
        DDSURFACEDESC *current_format, IDirectDrawPalette **palette,
        DDSURFACEDESC *desired_format, DWORD *flags)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);
    VIDEOINFOHEADER *video_info;

    TRACE("(%p/%p)->(%p,%p,%p,%p)\n", This, iface, current_format, palette, desired_format,
            flags);

    if (!This->input_pin->pin.pin.pConnectedTo)
        return MS_E_NOSTREAM;

    video_info = (VIDEOINFOHEADER *)This->input_pin->pin.pin.mtCurrent.pbFormat;

    if (current_format)
    {
        current_format->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS | (This->format.dwFlags & DDSD_PIXELFORMAT);
        current_format->dwWidth = video_info->bmiHeader.biWidth;
        current_format->dwHeight = abs(video_info->bmiHeader.biHeight);
        current_format->ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        current_format->ddpfPixelFormat = This->format.ddpfPixelFormat;
    }

    if (palette)
        *palette = NULL;

    if (desired_format)
    {
        desired_format->dwFlags = DDSD_WIDTH | DDSD_HEIGHT;
        desired_format->dwWidth = video_info->bmiHeader.biWidth;
        desired_format->dwHeight = abs(video_info->bmiHeader.biHeight);
    }

    if (flags)
        *flags = 0;

    return S_OK;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_SetFormat(IDirectDrawMediaStream *iface,
        const DDSURFACEDESC *pDDSurfaceDesc, IDirectDrawPalette *pDirectDrawPalette)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);

    TRACE("(%p)->(%p,%p)\n", iface, pDDSurfaceDesc, pDirectDrawPalette);

    if (!pDDSurfaceDesc)
        return E_POINTER;

    if (S_OK != ddrawmediastream_is_format_valid(pDDSurfaceDesc))
        return DDERR_INVALIDSURFACETYPE;

    if (This->input_pin->pin.pin.pConnectedTo)
    {
        if (S_OK != ddrawmediastream_is_media_type_compatible(&This->input_pin->pin.pin.mtCurrent, pDDSurfaceDesc))
        {
            if (FAILED(ddrawmediastream_reconnect(This, pDDSurfaceDesc)))
                return DDERR_INVALIDSURFACETYPE;
        }
    }

    This->format = *pDDSurfaceDesc;

    return S_OK;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetDirectDraw(IDirectDrawMediaStream *iface,
        IDirectDraw **ddraw)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);

    TRACE("(%p)->(%p)\n", iface, ddraw);

    *ddraw = NULL;
    if (!This->ddraw)
    {
        HRESULT hr = DirectDrawCreateEx(NULL, (void**)&This->ddraw, &IID_IDirectDraw7, NULL);
        if (FAILED(hr))
            return hr;
        IDirectDraw7_SetCooperativeLevel(This->ddraw, NULL, DDSCL_NORMAL);
    }

    return IDirectDraw7_QueryInterface(This->ddraw, &IID_IDirectDraw, (void**)ddraw);
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_SetDirectDraw(IDirectDrawMediaStream *iface,
        IDirectDraw *pDirectDraw)
{
    FIXME("(%p)->(%p) stub!\n", iface, pDirectDraw);

    return E_NOTIMPL;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_CreateSample(IDirectDrawMediaStream *iface,
        IDirectDrawSurface *surface, const RECT *rect, DWORD dwFlags,
        IDirectDrawStreamSample **ppSample)
{
    TRACE("(%p)->(%p,%s,%x,%p)\n", iface, surface, wine_dbgstr_rect(rect), dwFlags, ppSample);

    return ddrawstreamsample_create(iface, surface, rect, ppSample);
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetTimePerFrame(IDirectDrawMediaStream *iface,
        STREAM_TIME *pFrameTime)
{
    FIXME("(%p)->(%p) stub!\n", iface, pFrameTime);

    return E_NOTIMPL;
}

static const struct IDirectDrawMediaStreamVtbl DirectDrawMediaStreamImpl_IDirectDrawMediaStream_Vtbl =
{
    /*** IUnknown methods ***/
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_QueryInterface,
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_AddRef,
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_Release,
    /*** IMediaStream methods ***/
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetMultiMediaStream,
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetInformation,
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_SetSameFormat,
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_AllocateSample,
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_CreateSharedSample,
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_SendEndOfStream,
    /*** IDirectDrawMediaStream methods ***/
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetFormat,
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_SetFormat,
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetDirectDraw,
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_SetDirectDraw,
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_CreateSample,
    DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetTimePerFrame
};

typedef struct {
    struct list entry;
    IDirectDrawStreamSample *sample;
    HRESULT update_result;
    HANDLE update_complete_event;
} DirectDrawMediaStreamQueuedSample;

static inline DirectDrawMediaStreamInputPin *impl_from_DirectDrawMediaStreamInputPin_IPin(IPin *iface)
{
    return CONTAINING_RECORD(iface, DirectDrawMediaStreamInputPin, pin.pin.IPin_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI DirectDrawMediaStreamInputPin_IPin_QueryInterface(IPin *iface, REFIID riid, void **ret_iface)
{
    DirectDrawMediaStreamInputPin *This = impl_from_DirectDrawMediaStreamInputPin_IPin(iface);

    return IAMMediaStream_QueryInterface(&This->parent->IAMMediaStream_iface, riid, ret_iface);
}

static ULONG WINAPI DirectDrawMediaStreamInputPin_IPin_AddRef(IPin *iface)
{
    DirectDrawMediaStreamInputPin *This = impl_from_DirectDrawMediaStreamInputPin_IPin(iface);

    return IAMMediaStream_AddRef(&This->parent->IAMMediaStream_iface);
}

static ULONG WINAPI DirectDrawMediaStreamInputPin_IPin_Release(IPin *iface)
{
    DirectDrawMediaStreamInputPin *This = impl_from_DirectDrawMediaStreamInputPin_IPin(iface);

    return IAMMediaStream_Release(&This->parent->IAMMediaStream_iface);
}

/*** IPin methods ***/
static HRESULT WINAPI DirectDrawMediaStreamInputPin_IPin_EndOfStream(IPin *iface)
{
    DirectDrawMediaStreamInputPin *This = impl_from_DirectDrawMediaStreamInputPin_IPin(iface);
    HRESULT hr;

    TRACE("(%p/%p)->()\n", iface, This);

    hr = BaseInputPinImpl_EndOfStream(iface);
    if (FAILED(hr))
        return hr;

    EnterCriticalSection(This->pin.pin.pCritSec);
    while (!list_empty(&This->parent->sample_queue))
    {
        DirectDrawMediaStreamQueuedSample *output_sample =
            LIST_ENTRY(list_head(&This->parent->sample_queue), DirectDrawMediaStreamQueuedSample, entry);

        list_remove(&output_sample->entry);
        output_sample->update_result = MS_S_ENDOFSTREAM;
        SetEvent(output_sample->update_complete_event);
    }
    LeaveCriticalSection(This->pin.pin.pCritSec);

    return S_OK;
}

static const IPinVtbl DirectDrawMediaStreamInputPin_IPin_Vtbl =
{
    DirectDrawMediaStreamInputPin_IPin_QueryInterface,
    DirectDrawMediaStreamInputPin_IPin_AddRef,
    DirectDrawMediaStreamInputPin_IPin_Release,
    BaseInputPinImpl_Connect,
    BaseInputPinImpl_ReceiveConnection,
    BasePinImpl_Disconnect,
    BasePinImpl_ConnectedTo,
    BasePinImpl_ConnectionMediaType,
    BasePinImpl_QueryPinInfo,
    BasePinImpl_QueryDirection,
    BasePinImpl_QueryId,
    BaseInputPinImpl_QueryAccept,
    BasePinImpl_EnumMediaTypes,
    BasePinImpl_QueryInternalConnections,
    DirectDrawMediaStreamInputPin_IPin_EndOfStream,
    BaseInputPinImpl_BeginFlush,
    BaseInputPinImpl_EndFlush,
    BaseInputPinImpl_NewSegment,
};

static HRESULT WINAPI DirectDrawMediaStreamInputPin_CheckMediaType(BasePin *base, const AM_MEDIA_TYPE *media_type)
{
    DirectDrawMediaStreamInputPin *This = impl_from_DirectDrawMediaStreamInputPin_IPin(&base->IPin_iface);

    TRACE("(%p)->(%p)\n", This, media_type);

    return ddrawmediastream_is_media_type_compatible(media_type, &This->parent->format);
}

static LONG WINAPI DirectDrawMediaStreamInputPin_GetMediaTypeVersion(BasePin *base)
{
    return 0;
}

static HRESULT WINAPI DirectDrawMediaStreamInputPin_GetMediaType(BasePin *base, int index, AM_MEDIA_TYPE *media_type)
{
    DirectDrawMediaStreamInputPin *This = impl_from_DirectDrawMediaStreamInputPin_IPin(&base->IPin_iface);

    TRACE("(%p)->(%d,%p)\n", This, index, media_type);

    /* FIXME: Reset structure as we only fill majortype and minortype for now */
    ZeroMemory(media_type, sizeof(*media_type));

    media_type->majortype = MEDIATYPE_Video;

    switch (index)
    {
        case 0:
            media_type->subtype = MEDIASUBTYPE_RGB1;
            break;
        case 1:
            media_type->subtype = MEDIASUBTYPE_RGB4;
            break;
        case 2:
            media_type->subtype = MEDIASUBTYPE_RGB8;
            break;
        case 3:
            media_type->subtype = MEDIASUBTYPE_RGB565;
            break;
        case 4:
            media_type->subtype = MEDIASUBTYPE_RGB555;
            break;
        case 5:
            media_type->subtype = MEDIASUBTYPE_RGB24;
            break;
        case 6:
            media_type->subtype = MEDIASUBTYPE_RGB32;
            break;
        default:
            return S_FALSE;
    }

    return S_OK;
}

static HRESULT WINAPI DirectDrawMediaStreamInputPin_Receive(BaseInputPin *base, IMediaSample *sample)
{
    DirectDrawMediaStreamInputPin *This = impl_from_DirectDrawMediaStreamInputPin_IPin(&base->pin.IPin_iface);
    HRESULT hr;
    BYTE *sample_pointer = NULL;

    TRACE("(%p)->(%p)\n", This, sample);

    hr = IMediaSample_GetPointer(sample, &sample_pointer);
    if (FAILED(hr))
        return hr;

    EnterCriticalSection(This->pin.pin.pCritSec);
    for (;;)
    {
        if (This->parent->state == State_Stopped)
        {
            hr = VFW_E_WRONG_STATE;
            goto out_critical_section;
        }
        if (base->end_of_stream || base->flushing)
        {
            hr = S_FALSE;
            goto out_critical_section;
        }
        while (!list_empty(&This->parent->sample_queue))
        {
            DirectDrawMediaStreamQueuedSample *output_sample =
                LIST_ENTRY(list_head(&This->parent->sample_queue), DirectDrawMediaStreamQueuedSample, entry);
            IDirectDrawSurface *surface = NULL;
            RECT rect;
            DDSURFACEDESC desc = { sizeof(DDSURFACEDESC) };
            BITMAPINFOHEADER *bitmap_info = &((VIDEOINFOHEADER *)base->pin.mtCurrent.pbFormat)->bmiHeader;
            LONG stride = ((bitmap_info->biWidth * bitmap_info->biBitCount + 31) & ~31) >> 3;
            DWORD row_size;
            BYTE *row_pointer = sample_pointer;
            BYTE *output_row_pointer;
            DWORD row;
            if (bitmap_info->biHeight > 0)
            {
                row_pointer += stride * (bitmap_info->biHeight - 1);
                stride = -stride;
            }
            output_sample->update_result = IDirectDrawStreamSample_GetSurface(output_sample->sample, &surface, &rect);
            if (FAILED(output_sample->update_result))
                goto out_queue_entry;
            output_sample->update_result = IDirectDrawSurface_Lock(surface, &rect, &desc, DDLOCK_WAIT, NULL);
            if (FAILED(output_sample->update_result))
                goto out_surface;
            row_size = (rect.right - rect.left) * desc.ddpfPixelFormat.u1.dwRGBBitCount >> 3;
            output_row_pointer = (BYTE *)desc.lpSurface;
            for (row = rect.top; row < rect.bottom; ++row)
            {
                CopyMemory(output_row_pointer, row_pointer, row_size);
                row_pointer += stride;
                output_row_pointer += desc.u1.lPitch;
            }

            IDirectDrawSurface_Unlock(surface, NULL);

            output_sample->update_result = S_OK;

            IDirectDrawSurface_Release(surface);
            list_remove(&output_sample->entry);
            SetEvent(output_sample->update_complete_event);
            LeaveCriticalSection(This->pin.pin.pCritSec);
            return S_OK;

        out_surface:
            IDirectDrawSurface_Release(surface);
        out_queue_entry:
            list_remove(&output_sample->entry);
            SetEvent(output_sample->update_complete_event);
        }
        LeaveCriticalSection(This->pin.pin.pCritSec);
        WaitForSingleObject(This->parent->sample_queued_event, INFINITE);
        EnterCriticalSection(This->pin.pin.pCritSec);
    }
out_critical_section:
    LeaveCriticalSection(This->pin.pin.pCritSec);

    return hr;
}

static const BaseInputPinFuncTable DirectDrawMediaStreamInputPin_FuncTable =
{
    {
        DirectDrawMediaStreamInputPin_CheckMediaType,
        NULL,
        DirectDrawMediaStreamInputPin_GetMediaTypeVersion,
        DirectDrawMediaStreamInputPin_GetMediaType,
    },
    DirectDrawMediaStreamInputPin_Receive,
};

HRESULT ddrawmediastream_create(IMultiMediaStream *parent, const MSPID *purpose_id,
        STREAM_TYPE stream_type, IAMMediaStream **media_stream)
{
    DirectDrawMediaStreamImpl *object;
    PIN_INFO pin_info;
    HRESULT hr;

    TRACE("(%p,%s,%p)\n", parent, debugstr_guid(purpose_id), media_stream);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DirectDrawMediaStreamImpl));
    if (!object)
        return E_OUTOFMEMORY;

    object->IAMMediaStream_iface.lpVtbl = &DirectDrawMediaStreamImpl_IAMMediaStream_Vtbl;
    object->IDirectDrawMediaStream_iface.lpVtbl = &DirectDrawMediaStreamImpl_IDirectDrawMediaStream_Vtbl;
    object->ref = 1;

    InitializeCriticalSection(&object->critical_section);

    pin_info.pFilter = NULL;
    pin_info.dir = PINDIR_INPUT;
    pin_info.achName[0] = 'I';
    StringFromGUID2(purpose_id, pin_info.achName + 1, MAX_PIN_NAME - 1);
    hr = BaseInputPin_Construct(&DirectDrawMediaStreamInputPin_IPin_Vtbl,
        sizeof(DirectDrawMediaStreamInputPin), &pin_info, &DirectDrawMediaStreamInputPin_FuncTable,
        &object->critical_section, NULL, (IPin **)&object->input_pin);
    if (FAILED(hr))
        goto out_object;

    object->input_pin->parent = object;

    object->parent = parent;
    object->purpose_id = *purpose_id;
    object->stream_type = stream_type;
    object->sample_queued_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    list_init(&object->sample_queue);

    *media_stream = &object->IAMMediaStream_iface;

    return S_OK;

out_object:
    HeapFree(GetProcessHeap(), 0, object);

    return hr;
}

static HRESULT directdrawmediastream_queue_sample(IDirectDrawMediaStream *iface, DirectDrawMediaStreamQueuedSample *sample)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);
    HRESULT hr = S_OK;

    EnterCriticalSection(&This->critical_section);
    if (This->state == State_Stopped)
    {
        hr = MS_E_NOTRUNNING;
        goto out_critical_section;
    }
    if (This->input_pin->pin.end_of_stream)
    {
        hr = MS_S_ENDOFSTREAM;
        goto out_critical_section;
    }
    list_add_tail(&This->sample_queue, &sample->entry);
    SetEvent(This->sample_queued_event);
out_critical_section:
    LeaveCriticalSection(&This->critical_section);

    return hr;
}

struct AudioMediaStreamImpl;

typedef struct {
    BaseInputPin pin;
    struct AudioMediaStreamImpl *parent;
} AudioMediaStreamInputPin;

typedef struct AudioMediaStreamImpl {
    IAMMediaStream IAMMediaStream_iface;
    IAudioMediaStream IAudioMediaStream_iface;
    LONG ref;
    IMultiMediaStream* parent;
    MSPID purpose_id;
    STREAM_TYPE stream_type;
    AudioMediaStreamInputPin *input_pin;
    IFilterGraph *graph;
    CRITICAL_SECTION critical_section;
    WAVEFORMATEX format;
    FILTER_STATE state;
    struct list sample_queue;
    HANDLE sample_queued_event;
} AudioMediaStreamImpl;

static inline AudioMediaStreamImpl *impl_from_AudioMediaStream_IAMMediaStream(IAMMediaStream *iface)
{
    return CONTAINING_RECORD(iface, AudioMediaStreamImpl, IAMMediaStream_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_QueryInterface(IAMMediaStream *iface,
                                                        REFIID riid, void **ret_iface)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%s,%p)\n", iface, This, debugstr_guid(riid), ret_iface);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IMediaStream) ||
        IsEqualGUID(riid, &IID_IAMMediaStream))
    {
        IAMMediaStream_AddRef(iface);
        *ret_iface = iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IAudioMediaStream))
    {
        IAMMediaStream_AddRef(iface);
        *ret_iface = &This->IAudioMediaStream_iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IPin))
    {
        IAMMediaStream_AddRef(iface);
        *ret_iface = &This->input_pin->pin.pin.IPin_iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IMemInputPin))
    {
        IAMMediaStream_AddRef(iface);
        *ret_iface = &This->input_pin->pin.IMemInputPin_iface;
        return S_OK;
    }

    ERR("(%p)->(%s,%p),not found\n", This, debugstr_guid(riid), ret_iface);
    return E_NOINTERFACE;
}

static ULONG WINAPI AudioMediaStreamImpl_IAMMediaStream_AddRef(IAMMediaStream *iface)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p/%p)->(): new ref = %u\n", iface, This, ref);

    return ref;
}

static ULONG WINAPI AudioMediaStreamImpl_IAMMediaStream_Release(IAMMediaStream *iface)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p/%p)->(): new ref = %u\n", iface, This, ref);

    if (!ref)
    {
        BaseInputPin_Destroy((BaseInputPin *)This->input_pin);
        DeleteCriticalSection(&This->critical_section);
        HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

/*** IMediaStream methods ***/
static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_GetMultiMediaStream(IAMMediaStream *iface,
        IMultiMediaStream** multi_media_stream)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", This, iface, multi_media_stream);

    if (!multi_media_stream)
        return E_POINTER;

    IMultiMediaStream_AddRef(This->parent);
    *multi_media_stream = This->parent;

    return S_OK;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_GetInformation(IAMMediaStream *iface,
        MSPID *purpose_id, STREAM_TYPE *type)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%p,%p)\n", This, iface, purpose_id, type);

    if (purpose_id)
        *purpose_id = This->purpose_id;
    if (type)
        *type = This->stream_type;

    return S_OK;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_SetSameFormat(IAMMediaStream *iface,
        IMediaStream *pStreamThatHasDesiredFormat, DWORD flags)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%p,%x) stub!\n", This, iface, pStreamThatHasDesiredFormat, flags);

    return S_FALSE;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_AllocateSample(IAMMediaStream *iface,
        DWORD flags, IStreamSample **sample)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%x,%p) stub!\n", This, iface, flags, sample);

    return S_FALSE;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_CreateSharedSample(IAMMediaStream *iface,
        IStreamSample *existing_sample, DWORD flags, IStreamSample **sample)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%p,%x,%p) stub!\n", This, iface, existing_sample, flags, sample);

    return S_FALSE;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_SendEndOfStream(IAMMediaStream *iface, DWORD flags)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%x) stub!\n", This, iface, flags);

    return S_FALSE;
}

/*** IAMMediaStream methods ***/
static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_Initialize(IAMMediaStream *iface, IUnknown *source_object, DWORD flags,
                                                    REFMSPID purpose_id, const STREAM_TYPE stream_type)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%p,%x,%p,%u) stub!\n", This, iface, source_object, flags, purpose_id, stream_type);

    return S_FALSE;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_SetState(IAMMediaStream *iface, FILTER_STATE state)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);
    HRESULT hr;

    TRACE("(%p/%p)->(%u)\n", This, iface, state);

    EnterCriticalSection(&This->critical_section);

    if (This->state == state)
        goto out_critical_section;

    switch (state)
    {
    case State_Stopped:
        SetEvent(This->sample_queued_event);
        break;
    case State_Paused:
    case State_Running:
        if (State_Stopped == This->state)
            This->input_pin->pin.end_of_stream = FALSE;
        break;
    default:
        hr = E_INVALIDARG;
        goto out_critical_section;
    }

    This->state = state;

out_critical_section:
    LeaveCriticalSection(&This->critical_section);

    return hr;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_JoinAMMultiMediaStream(IAMMediaStream *iface, IAMMultiMediaStream *am_multi_media_stream)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%p) stub!\n", This, iface, am_multi_media_stream);

    return S_FALSE;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_JoinFilter(IAMMediaStream *iface, IMediaStreamFilter *media_stream_filter)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", This, iface, media_stream_filter);

    This->input_pin->pin.pin.pinInfo.pFilter = (IBaseFilter *)media_stream_filter;

    return S_OK;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_JoinFilterGraph(IAMMediaStream *iface, IFilterGraph *filtergraph)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", This, iface, filtergraph);

    This->graph = filtergraph;

    return S_OK;
}

static const struct IAMMediaStreamVtbl AudioMediaStreamImpl_IAMMediaStream_Vtbl =
{
    /*** IUnknown methods ***/
    AudioMediaStreamImpl_IAMMediaStream_QueryInterface,
    AudioMediaStreamImpl_IAMMediaStream_AddRef,
    AudioMediaStreamImpl_IAMMediaStream_Release,
    /*** IMediaStream methods ***/
    AudioMediaStreamImpl_IAMMediaStream_GetMultiMediaStream,
    AudioMediaStreamImpl_IAMMediaStream_GetInformation,
    AudioMediaStreamImpl_IAMMediaStream_SetSameFormat,
    AudioMediaStreamImpl_IAMMediaStream_AllocateSample,
    AudioMediaStreamImpl_IAMMediaStream_CreateSharedSample,
    AudioMediaStreamImpl_IAMMediaStream_SendEndOfStream,
    /*** IAMMediaStream methods ***/
    AudioMediaStreamImpl_IAMMediaStream_Initialize,
    AudioMediaStreamImpl_IAMMediaStream_SetState,
    AudioMediaStreamImpl_IAMMediaStream_JoinAMMultiMediaStream,
    AudioMediaStreamImpl_IAMMediaStream_JoinFilter,
    AudioMediaStreamImpl_IAMMediaStream_JoinFilterGraph
};

static HRESULT audiomediastream_is_media_type_compatible(const AM_MEDIA_TYPE *media_type, const WAVEFORMATEX *format)
{
    const WAVEFORMATEX *media_type_format;

    if (!IsEqualGUID(&media_type->majortype, &MEDIATYPE_Audio))
        return S_FALSE;

    if (!IsEqualGUID(&media_type->subtype, &MEDIASUBTYPE_PCM))
        return S_FALSE;

    if (!IsEqualGUID(&media_type->formattype, &FORMAT_WaveFormatEx))
        return S_FALSE;

    if (!media_type->pbFormat)
        return S_FALSE;

    if (media_type->cbFormat < sizeof(WAVEFORMATEX))
        return S_FALSE;

    media_type_format = (const WAVEFORMATEX *)media_type->pbFormat;

    if (media_type_format->wFormatTag != WAVE_FORMAT_PCM)
        return S_FALSE;

    if (format->wFormatTag == WAVE_FORMAT_PCM)
    {
        if (media_type_format->nChannels != format->nChannels)
            return S_FALSE;

        if (media_type_format->nSamplesPerSec != format->nSamplesPerSec)
            return S_FALSE;

        if (media_type_format->nAvgBytesPerSec != format->nAvgBytesPerSec)
            return S_FALSE;

        if (media_type_format->nBlockAlign != format->nBlockAlign)
            return S_FALSE;

        if (media_type_format->wBitsPerSample != format->wBitsPerSample)
            return S_FALSE;
    }

    return S_OK;
}

static HRESULT audiomediastream_is_format_valid(const WAVEFORMATEX *format)
{
    if (format->wFormatTag != WAVE_FORMAT_PCM)
        return S_FALSE;
    return S_OK;
}

static inline AudioMediaStreamImpl *impl_from_IAudioMediaStream(IAudioMediaStream *iface)
{
    return CONTAINING_RECORD(iface, AudioMediaStreamImpl, IAudioMediaStream_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_QueryInterface(IAudioMediaStream *iface,
        REFIID riid, void **ret_iface)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    TRACE("(%p/%p)->(%s,%p)\n", iface, This, debugstr_guid(riid), ret_iface);
    return IAMMediaStream_QueryInterface(&This->IAMMediaStream_iface, riid, ret_iface);
}

static ULONG WINAPI AudioMediaStreamImpl_IAudioMediaStream_AddRef(IAudioMediaStream *iface)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    TRACE("(%p/%p)\n", iface, This);
    return IAMMediaStream_AddRef(&This->IAMMediaStream_iface);
}

static ULONG WINAPI AudioMediaStreamImpl_IAudioMediaStream_Release(IAudioMediaStream *iface)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    TRACE("(%p/%p)\n", iface, This);
    return IAMMediaStream_Release(&This->IAMMediaStream_iface);
}

/*** IMediaStream methods ***/
static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_GetMultiMediaStream(IAudioMediaStream *iface,
        IMultiMediaStream **multi_media_stream)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", iface, This, multi_media_stream);

    if (!multi_media_stream)
        return E_POINTER;

    IMultiMediaStream_AddRef(This->parent);
    *multi_media_stream = This->parent;

    return S_OK;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_GetInformation(IAudioMediaStream *iface,
        MSPID *purpose_id, STREAM_TYPE *type)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);

    TRACE("(%p/%p)->(%p,%p)\n", iface, This, purpose_id, type);

    if (purpose_id)
        *purpose_id = This->purpose_id;
    if (type)
        *type = This->stream_type;

    return S_OK;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_SetSameFormat(IAudioMediaStream *iface,
        IMediaStream *stream_format, DWORD flags)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);

    FIXME("(%p/%p)->(%p,%x) stub!\n", iface, This, stream_format, flags);

    return S_FALSE;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_AllocateSample(IAudioMediaStream *iface,
        DWORD flags, IStreamSample **sample)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);

    FIXME("(%p/%p)->(%x,%p) stub!\n", iface, This, flags, sample);

    return S_FALSE;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_CreateSharedSample(IAudioMediaStream *iface,
        IStreamSample *existing_sample, DWORD flags, IStreamSample **sample)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);

    FIXME("(%p/%p)->(%p,%x,%p) stub!\n", iface, This, existing_sample, flags, sample);

    return S_FALSE;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_SendEndOfStream(IAudioMediaStream *iface,
        DWORD flags)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);

    FIXME("(%p/%p)->(%x) stub!\n", iface, This, flags);

    return S_FALSE;
}

/*** IAudioMediaStream methods ***/
static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_GetFormat(IAudioMediaStream *iface, WAVEFORMATEX *wave_format_current)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", iface, This, wave_format_current);

    if (!wave_format_current)
        return E_POINTER;

    if (!This->input_pin->pin.pin.mtCurrent.pbFormat)
        return MS_E_NOSTREAM;

    *wave_format_current = *(const WAVEFORMATEX *)This->input_pin->pin.pin.mtCurrent.pbFormat;

    return S_OK;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_SetFormat(IAudioMediaStream *iface, const WAVEFORMATEX *wave_format)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", iface, This, wave_format);

    if (!wave_format)
        return E_POINTER;

    if (S_OK != audiomediastream_is_format_valid(wave_format))
        return E_INVALIDARG;

    if (This->input_pin->pin.pin.pConnectedTo)
    {
        if (S_OK != audiomediastream_is_media_type_compatible(&This->input_pin->pin.pin.mtCurrent, wave_format))
            return E_INVALIDARG;
    }

    This->format = *wave_format;

    return S_OK;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_CreateSample(IAudioMediaStream *iface, IAudioData *audio_data,
                                                         DWORD flags, IAudioStreamSample **sample)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);

    TRACE("(%p/%p)->(%p,%u,%p)\n", iface, This, audio_data, flags, sample);

    if (!audio_data)
        return E_POINTER;

    return audiostreamsample_create(iface, audio_data, sample);
}

static const struct IAudioMediaStreamVtbl AudioMediaStreamImpl_IAudioMediaStream_Vtbl =
{
    /*** IUnknown methods ***/
    AudioMediaStreamImpl_IAudioMediaStream_QueryInterface,
    AudioMediaStreamImpl_IAudioMediaStream_AddRef,
    AudioMediaStreamImpl_IAudioMediaStream_Release,
    /*** IMediaStream methods ***/
    AudioMediaStreamImpl_IAudioMediaStream_GetMultiMediaStream,
    AudioMediaStreamImpl_IAudioMediaStream_GetInformation,
    AudioMediaStreamImpl_IAudioMediaStream_SetSameFormat,
    AudioMediaStreamImpl_IAudioMediaStream_AllocateSample,
    AudioMediaStreamImpl_IAudioMediaStream_CreateSharedSample,
    AudioMediaStreamImpl_IAudioMediaStream_SendEndOfStream,
    /*** IAudioMediaStream methods ***/
    AudioMediaStreamImpl_IAudioMediaStream_GetFormat,
    AudioMediaStreamImpl_IAudioMediaStream_SetFormat,
    AudioMediaStreamImpl_IAudioMediaStream_CreateSample
};

typedef struct {
    struct list entry;
    IAudioStreamSample *sample;
    HRESULT update_result;
    HANDLE update_complete_event;
} AudioMediaStreamQueuedSample;

static inline AudioMediaStreamInputPin *impl_from_AudioMediaStreamInputPin_IPin(IPin *iface)
{
    return CONTAINING_RECORD(iface, AudioMediaStreamInputPin, pin.pin.IPin_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI AudioMediaStreamInputPin_IPin_QueryInterface(IPin *iface, REFIID riid, void **ret_iface)
{
    AudioMediaStreamInputPin *This = impl_from_AudioMediaStreamInputPin_IPin(iface);

    return IAMMediaStream_QueryInterface(&This->parent->IAMMediaStream_iface, riid, ret_iface);
}

static ULONG WINAPI AudioMediaStreamInputPin_IPin_AddRef(IPin *iface)
{
    AudioMediaStreamInputPin *This = impl_from_AudioMediaStreamInputPin_IPin(iface);

    return IAMMediaStream_AddRef(&This->parent->IAMMediaStream_iface);
}

static ULONG WINAPI AudioMediaStreamInputPin_IPin_Release(IPin *iface)
{
    AudioMediaStreamInputPin *This = impl_from_AudioMediaStreamInputPin_IPin(iface);

    return IAMMediaStream_Release(&This->parent->IAMMediaStream_iface);
}

/*** IPin methods ***/
static HRESULT WINAPI AudioMediaStreamInputPin_IPin_EndOfStream(IPin *iface)
{
    AudioMediaStreamInputPin *This = impl_from_AudioMediaStreamInputPin_IPin(iface);
    HRESULT hr;

    TRACE("(%p/%p)->()\n", iface, This);

    hr = BaseInputPinImpl_EndOfStream(iface);
    if (FAILED(hr))
        return hr;

    EnterCriticalSection(This->pin.pin.pCritSec);
    while (!list_empty(&This->parent->sample_queue))
    {
        AudioMediaStreamQueuedSample *output_sample =
            LIST_ENTRY(list_head(&This->parent->sample_queue), AudioMediaStreamQueuedSample, entry);
        IAudioData *audio_data = NULL;
        DWORD actual_length = 0;

        list_remove(&output_sample->entry);

        output_sample->update_result = IAudioStreamSample_GetAudioData(output_sample->sample, &audio_data);
        if (SUCCEEDED(output_sample->update_result))
        {
            output_sample->update_result = IAudioData_GetInfo(audio_data, NULL, NULL, &actual_length);
            if (SUCCEEDED(output_sample->update_result))
                output_sample->update_result = actual_length > 0 ? S_OK : MS_S_ENDOFSTREAM;
            IAudioData_Release(audio_data);
        }

        SetEvent(output_sample->update_complete_event);
    }
    LeaveCriticalSection(This->pin.pin.pCritSec);

    return S_OK;
}

static const IPinVtbl AudioMediaStreamInputPin_IPin_Vtbl =
{
    AudioMediaStreamInputPin_IPin_QueryInterface,
    AudioMediaStreamInputPin_IPin_AddRef,
    AudioMediaStreamInputPin_IPin_Release,
    BaseInputPinImpl_Connect,
    BaseInputPinImpl_ReceiveConnection,
    BasePinImpl_Disconnect,
    BasePinImpl_ConnectedTo,
    BasePinImpl_ConnectionMediaType,
    BasePinImpl_QueryPinInfo,
    BasePinImpl_QueryDirection,
    BasePinImpl_QueryId,
    BaseInputPinImpl_QueryAccept,
    BasePinImpl_EnumMediaTypes,
    BasePinImpl_QueryInternalConnections,
    AudioMediaStreamInputPin_IPin_EndOfStream,
    BaseInputPinImpl_BeginFlush,
    BaseInputPinImpl_EndFlush,
    BaseInputPinImpl_NewSegment,
};

static HRESULT WINAPI AudioMediaStreamInputPin_CheckMediaType(BasePin *base, const AM_MEDIA_TYPE *media_type)
{
    AudioMediaStreamInputPin *This = impl_from_AudioMediaStreamInputPin_IPin(&base->IPin_iface);

    TRACE("(%p)->(%p)\n", This, media_type);

    return audiomediastream_is_media_type_compatible(media_type, &This->parent->format);
}

static LONG WINAPI AudioMediaStreamInputPin_GetMediaTypeVersion(BasePin *base)
{
    return 0;
}

static HRESULT WINAPI AudioMediaStreamInputPin_GetMediaType(BasePin *base, int index, AM_MEDIA_TYPE *media_type)
{
    AudioMediaStreamInputPin *This = impl_from_AudioMediaStreamInputPin_IPin(&base->IPin_iface);

    TRACE("(%p)->(%d,%p)\n", This, index, media_type);

    /* FIXME: Reset structure as we only fill majortype and minortype for now */
    ZeroMemory(media_type, sizeof(*media_type));

    if (index)
        return S_FALSE;

    media_type->majortype = MEDIATYPE_Audio;
    media_type->subtype = MEDIASUBTYPE_PCM;

    return S_OK;
}

static HRESULT WINAPI AudioMediaStreamInputPin_Receive(BaseInputPin *base, IMediaSample *sample)
{
    AudioMediaStreamInputPin *This = impl_from_AudioMediaStreamInputPin_IPin(&base->pin.IPin_iface);
    HRESULT hr;
    DWORD input_length = 0;
    BYTE *input_pointer = NULL;
    DWORD input_position = 0;

    TRACE("(%p)->(%p)\n", This, sample);

    hr = IMediaSample_GetPointer(sample, &input_pointer);
    if (FAILED(hr))
        return hr;
    input_length = IMediaSample_GetActualDataLength(sample);

    EnterCriticalSection(This->pin.pin.pCritSec);
    for (;;)
    {
        if (This->parent->state == State_Stopped)
        {
            hr = VFW_E_WRONG_STATE;
            goto out_critical_section;
        }
        if (base->end_of_stream || base->flushing)
        {
            hr = S_FALSE;
            goto out_critical_section;
        }
        while (!list_empty(&This->parent->sample_queue))
        {
            AudioMediaStreamQueuedSample *output_sample =
                LIST_ENTRY(list_head(&This->parent->sample_queue), AudioMediaStreamQueuedSample, entry);
            IAudioData *audio_data = NULL;
            DWORD output_length = 0;
            BYTE *output_pointer = NULL;
            DWORD output_position = 0;
            DWORD advance = 0;

            output_sample->update_result = IAudioStreamSample_GetAudioData(output_sample->sample, &audio_data);
            if (FAILED(output_sample->update_result))
                goto out_queue_entry;
            output_sample->update_result = IAudioData_GetInfo(audio_data, &output_length, &output_pointer, &output_position);
            if (FAILED(output_sample->update_result))
                goto out_audio_data;

            advance = min(input_length - input_position, output_length - output_position);
            CopyMemory(&output_pointer[output_position], &input_pointer[input_position], advance);

            input_position += advance;
            output_position += advance;

            output_sample->update_result = IAudioData_SetActual(audio_data, output_position);
            if (FAILED(output_sample->update_result))
                goto out_audio_data;

            IAudioData_Release(audio_data);

            if (output_position == output_length)
            {
                output_sample->update_result = S_OK;

                list_remove(&output_sample->entry);
                SetEvent(output_sample->update_complete_event);
            }
            if (input_position == input_length)
            {
                hr = S_OK;
                goto out_critical_section;
            }

            continue;

        out_audio_data:
            IAudioData_Release(audio_data);
        out_queue_entry:
            list_remove(&output_sample->entry);
            SetEvent(output_sample->update_complete_event);
        }
        LeaveCriticalSection(This->pin.pin.pCritSec);
        WaitForSingleObject(This->parent->sample_queued_event, INFINITE);
        EnterCriticalSection(This->pin.pin.pCritSec);
    }
out_critical_section:
    LeaveCriticalSection(This->pin.pin.pCritSec);

    return hr;
}

static const BaseInputPinFuncTable AudioMediaStreamInputPin_FuncTable =
{
    {
        AudioMediaStreamInputPin_CheckMediaType,
        NULL,
        AudioMediaStreamInputPin_GetMediaTypeVersion,
        AudioMediaStreamInputPin_GetMediaType,
    },
    AudioMediaStreamInputPin_Receive,
};

HRESULT audiomediastream_create(IMultiMediaStream *parent, const MSPID *purpose_id,
        STREAM_TYPE stream_type, IAMMediaStream **media_stream)
{
    AudioMediaStreamImpl *object;
    PIN_INFO pin_info;
    HRESULT hr;

    TRACE("(%p,%s,%p)\n", parent, debugstr_guid(purpose_id), media_stream);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(AudioMediaStreamImpl));
    if (!object)
        return E_OUTOFMEMORY;

    object->IAMMediaStream_iface.lpVtbl = &AudioMediaStreamImpl_IAMMediaStream_Vtbl;
    object->IAudioMediaStream_iface.lpVtbl = &AudioMediaStreamImpl_IAudioMediaStream_Vtbl;
    object->ref = 1;

    InitializeCriticalSection(&object->critical_section);

    pin_info.pFilter = NULL;
    pin_info.dir = PINDIR_INPUT;
    pin_info.achName[0] = 'I';
    StringFromGUID2(purpose_id, pin_info.achName + 1, MAX_PIN_NAME - 1);
    hr = BaseInputPin_Construct(&AudioMediaStreamInputPin_IPin_Vtbl,
        sizeof(AudioMediaStreamInputPin), &pin_info, &AudioMediaStreamInputPin_FuncTable,
        &object->critical_section, NULL, (IPin **)&object->input_pin);
    if (FAILED(hr))
        goto out_object;

    object->input_pin->parent = object;

    object->parent = parent;
    object->purpose_id = *purpose_id;
    object->stream_type = stream_type;
    object->sample_queued_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    list_init(&object->sample_queue);

    *media_stream = &object->IAMMediaStream_iface;

    return S_OK;

out_object:
    HeapFree(GetProcessHeap(), 0, object);

    return hr;
}

static HRESULT audiomediastream_queue_sample(IAudioMediaStream *iface, AudioMediaStreamQueuedSample *sample)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    HRESULT hr = S_OK;

    EnterCriticalSection(&This->critical_section);
    if (This->state == State_Stopped)
    {
        hr = MS_E_NOTRUNNING;
        goto out_critical_section;
    }
    if (This->input_pin->pin.end_of_stream)
    {
        hr = MS_S_ENDOFSTREAM;
        goto out_critical_section;
    }
    list_add_tail(&This->sample_queue, &sample->entry);
    SetEvent(This->sample_queued_event);
out_critical_section:
    LeaveCriticalSection(&This->critical_section);

    return hr;
}

typedef struct {
    IDirectDrawStreamSample IDirectDrawStreamSample_iface;
    LONG ref;
    IDirectDrawMediaStream *parent;
    IDirectDrawSurface *surface;
    RECT rect;
    DirectDrawMediaStreamQueuedSample queue_entry;
} IDirectDrawStreamSampleImpl;

static inline IDirectDrawStreamSampleImpl *impl_from_IDirectDrawStreamSample(IDirectDrawStreamSample *iface)
{
    return CONTAINING_RECORD(iface, IDirectDrawStreamSampleImpl, IDirectDrawStreamSample_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI IDirectDrawStreamSampleImpl_QueryInterface(IDirectDrawStreamSample *iface,
        REFIID riid, void **ret_iface)
{
    TRACE("(%p)->(%s,%p)\n", iface, debugstr_guid(riid), ret_iface);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IStreamSample) ||
        IsEqualGUID(riid, &IID_IDirectDrawStreamSample))
    {
        IDirectDrawStreamSample_AddRef(iface);
        *ret_iface = iface;
        return S_OK;
    }

    *ret_iface = NULL;

    ERR("(%p)->(%s,%p),not found\n", iface, debugstr_guid(riid), ret_iface);
    return E_NOINTERFACE;
}

static ULONG WINAPI IDirectDrawStreamSampleImpl_AddRef(IDirectDrawStreamSample *iface)
{
    IDirectDrawStreamSampleImpl *This = impl_from_IDirectDrawStreamSample(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p)->(): new ref = %u\n", iface, ref);

    return ref;
}

static ULONG WINAPI IDirectDrawStreamSampleImpl_Release(IDirectDrawStreamSample *iface)
{
    IDirectDrawStreamSampleImpl *This = impl_from_IDirectDrawStreamSample(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p)->(): new ref = %u\n", iface, ref);

    if (!ref)
    {
        if (This->queue_entry.update_complete_event)
            CloseHandle(This->queue_entry.update_complete_event);
        if (This->surface)
            IDirectDrawSurface_Release(This->surface);
        IDirectDrawMediaStream_Release(This->parent);
        HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

/*** IStreamSample methods ***/
static HRESULT WINAPI IDirectDrawStreamSampleImpl_GetMediaStream(IDirectDrawStreamSample *iface, IMediaStream **media_stream)
{
    FIXME("(%p)->(%p): stub\n", iface, media_stream);

    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectDrawStreamSampleImpl_GetSampleTimes(IDirectDrawStreamSample *iface, STREAM_TIME *start_time,
                                                                 STREAM_TIME *end_time, STREAM_TIME *current_time)
{
    FIXME("(%p)->(%p,%p,%p): stub\n", iface, start_time, end_time, current_time);

    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectDrawStreamSampleImpl_SetSampleTimes(IDirectDrawStreamSample *iface, const STREAM_TIME *start_time,
                                                                 const STREAM_TIME *end_time)
{
    FIXME("(%p)->(%p,%p): stub\n", iface, start_time, end_time);

    return E_NOTIMPL;
}

static HRESULT WINAPI IDirectDrawStreamSampleImpl_Update(IDirectDrawStreamSample *iface, DWORD flags, HANDLE event,
                                                         PAPCFUNC func_APC, DWORD APC_data)
{
    IDirectDrawStreamSampleImpl *This = impl_from_IDirectDrawStreamSample(iface);
    HRESULT hr;

    TRACE("(%p)->(%x,%p,%p,%u)\n", iface, flags, event, func_APC, APC_data);

    hr = directdrawmediastream_queue_sample(This->parent, &This->queue_entry);
    if (hr != S_OK)
        return hr;

    WaitForSingleObject(This->queue_entry.update_complete_event, INFINITE);

    return This->queue_entry.update_result;
}

static HRESULT WINAPI IDirectDrawStreamSampleImpl_CompletionStatus(IDirectDrawStreamSample *iface, DWORD flags, DWORD milliseconds)
{
    FIXME("(%p)->(%x,%u): stub\n", iface, flags, milliseconds);

    return E_NOTIMPL;
}

/*** IDirectDrawStreamSample methods ***/
static HRESULT WINAPI IDirectDrawStreamSampleImpl_GetSurface(IDirectDrawStreamSample *iface, IDirectDrawSurface **ddraw_surface,
                                                             RECT *rect)
{
    IDirectDrawStreamSampleImpl *This = impl_from_IDirectDrawStreamSample(iface);

    TRACE("(%p)->(%p,%p)\n", iface, ddraw_surface, rect);

    if (ddraw_surface)
    {
        *ddraw_surface = This->surface;
        if (*ddraw_surface)
            IDirectDrawSurface_AddRef(*ddraw_surface);
    }

    if (rect)
        *rect = This->rect;

    return S_OK;
}

static HRESULT WINAPI IDirectDrawStreamSampleImpl_SetRect(IDirectDrawStreamSample *iface, const RECT *rect)
{
    FIXME("(%p)->(%p): stub\n", iface, rect);

    return E_NOTIMPL;
}

static const struct IDirectDrawStreamSampleVtbl DirectDrawStreamSample_Vtbl =
{
    /*** IUnknown methods ***/
    IDirectDrawStreamSampleImpl_QueryInterface,
    IDirectDrawStreamSampleImpl_AddRef,
    IDirectDrawStreamSampleImpl_Release,
    /*** IStreamSample methods ***/
    IDirectDrawStreamSampleImpl_GetMediaStream,
    IDirectDrawStreamSampleImpl_GetSampleTimes,
    IDirectDrawStreamSampleImpl_SetSampleTimes,
    IDirectDrawStreamSampleImpl_Update,
    IDirectDrawStreamSampleImpl_CompletionStatus,
    /*** IDirectDrawStreamSample methods ***/
    IDirectDrawStreamSampleImpl_GetSurface,
    IDirectDrawStreamSampleImpl_SetRect
};

static HRESULT ddrawstreamsample_create(IDirectDrawMediaStream *parent, IDirectDrawSurface *surface,
    const RECT *rect, IDirectDrawStreamSample **ddraw_stream_sample)
{
    IDirectDrawStreamSampleImpl *object;
    HRESULT hr;

    TRACE("(%p)\n", ddraw_stream_sample);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if (!object)
        return E_OUTOFMEMORY;

    object->IDirectDrawStreamSample_iface.lpVtbl = &DirectDrawStreamSample_Vtbl;
    object->ref = 1;
    object->parent = parent;
    IDirectDrawMediaStream_AddRef(object->parent);

    if (surface)
    {
        object->surface = surface;
        IDirectDrawSurface_AddRef(surface);
    }
    else
    {
        DDSURFACEDESC desc;
        IDirectDraw *ddraw;

        hr = IDirectDrawMediaStream_GetDirectDraw(parent, &ddraw);
        if (FAILED(hr))
        {
            IDirectDrawStreamSample_Release(&object->IDirectDrawStreamSample_iface);
            return hr;
        }

        desc.dwSize = sizeof(desc);
        desc.dwFlags = DDSD_CAPS|DDSD_HEIGHT|DDSD_WIDTH|DDSD_PIXELFORMAT;
        desc.dwHeight = 100;
        desc.dwWidth = 100;
        desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
        desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
        desc.ddpfPixelFormat.u1.dwRGBBitCount = 32;
        desc.ddpfPixelFormat.u2.dwRBitMask = 0xff0000;
        desc.ddpfPixelFormat.u3.dwGBitMask = 0x00ff00;
        desc.ddpfPixelFormat.u4.dwBBitMask = 0x0000ff;
        desc.ddpfPixelFormat.u5.dwRGBAlphaBitMask = 0;
        desc.ddsCaps.dwCaps = DDSCAPS_SYSTEMMEMORY|DDSCAPS_OFFSCREENPLAIN;
        desc.lpSurface = NULL;

        hr = IDirectDraw_CreateSurface(ddraw, &desc, &object->surface, NULL);
        IDirectDraw_Release(ddraw);
        if (FAILED(hr))
        {
            ERR("failed to create surface, 0x%08x\n", hr);
            IDirectDrawStreamSample_Release(&object->IDirectDrawStreamSample_iface);
            return hr;
        }
    }

    {
        DDSURFACEDESC desc = { sizeof(desc) };
        hr = IDirectDrawSurface_GetSurfaceDesc(object->surface, &desc);
        if (FAILED(hr))
        {
            IDirectDrawStreamSample_Release(&object->IDirectDrawStreamSample_iface);
            return hr;
        }
        if (rect)
        {
            object->rect = *rect;
            desc.dwWidth = rect->right - rect->left;
            desc.dwHeight = rect->bottom - rect->top;
            desc.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT;
        }
        else
        {
            SetRect(&object->rect, 0, 0, desc.dwWidth, desc.dwHeight);
        }
        hr = IDirectDrawMediaStream_SetFormat(parent, &desc, NULL);
        if (FAILED(hr))
        {
            IDirectDrawStreamSample_Release(&object->IDirectDrawStreamSample_iface);
            return hr;
        }
    }

    object->queue_entry.sample = &object->IDirectDrawStreamSample_iface;
    object->queue_entry.update_complete_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    *ddraw_stream_sample = &object->IDirectDrawStreamSample_iface;

    return S_OK;
}

typedef struct {
    IAudioStreamSample IAudioStreamSample_iface;
    LONG ref;
    IAudioMediaStream *parent;
    IAudioData *audio_data;
    AudioMediaStreamQueuedSample queue_entry;
} IAudioStreamSampleImpl;

static inline IAudioStreamSampleImpl *impl_from_IAudioStreamSample(IAudioStreamSample *iface)
{
    return CONTAINING_RECORD(iface, IAudioStreamSampleImpl, IAudioStreamSample_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI IAudioStreamSampleImpl_QueryInterface(IAudioStreamSample *iface,
        REFIID riid, void **ret_iface)
{
    TRACE("(%p)->(%s,%p)\n", iface, debugstr_guid(riid), ret_iface);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IStreamSample) ||
        IsEqualGUID(riid, &IID_IAudioStreamSample))
    {
        IAudioStreamSample_AddRef(iface);
        *ret_iface = iface;
        return S_OK;
    }

    *ret_iface = NULL;

    ERR("(%p)->(%s,%p),not found\n", iface, debugstr_guid(riid), ret_iface);
    return E_NOINTERFACE;
}

static ULONG WINAPI IAudioStreamSampleImpl_AddRef(IAudioStreamSample *iface)
{
    IAudioStreamSampleImpl *This = impl_from_IAudioStreamSample(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p)->(): new ref = %u\n", iface, ref);

    return ref;
}

static ULONG WINAPI IAudioStreamSampleImpl_Release(IAudioStreamSample *iface)
{
    IAudioStreamSampleImpl *This = impl_from_IAudioStreamSample(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p)->(): new ref = %u\n", iface, ref);

    if (!ref)
    {
        if (This->audio_data)
            IAudioData_Release(This->audio_data);
        if (This->queue_entry.update_complete_event)
            CloseHandle(This->queue_entry.update_complete_event);
        HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

/*** IStreamSample methods ***/
static HRESULT WINAPI IAudioStreamSampleImpl_GetMediaStream(IAudioStreamSample *iface, IMediaStream **media_stream)
{
    FIXME("(%p)->(%p): stub\n", iface, media_stream);

    return E_NOTIMPL;
}

static HRESULT WINAPI IAudioStreamSampleImpl_GetSampleTimes(IAudioStreamSample *iface, STREAM_TIME *start_time,
                                                                 STREAM_TIME *end_time, STREAM_TIME *current_time)
{
    FIXME("(%p)->(%p,%p,%p): stub\n", iface, start_time, end_time, current_time);

    return E_NOTIMPL;
}

static HRESULT WINAPI IAudioStreamSampleImpl_SetSampleTimes(IAudioStreamSample *iface, const STREAM_TIME *start_time,
                                                                 const STREAM_TIME *end_time)
{
    FIXME("(%p)->(%p,%p): stub\n", iface, start_time, end_time);

    return E_NOTIMPL;
}

static HRESULT WINAPI IAudioStreamSampleImpl_Update(IAudioStreamSample *iface, DWORD flags, HANDLE event,
                                                         PAPCFUNC func_APC, DWORD APC_data)
{
    IAudioStreamSampleImpl *This = impl_from_IAudioStreamSample(iface);
    IAudioData *audio_data = NULL;
    HRESULT hr;

    TRACE("(%p)->(%x,%p,%p,%u)\n", iface, flags, event, func_APC, APC_data);

    TRACE("FUCK0\n");

    hr = IAudioStreamSample_GetAudioData(iface, &audio_data);
    if (FAILED(hr))
        return hr;

    TRACE("FUCK1\n");

    hr = IAudioData_SetActual(audio_data, 0);
    if (FAILED(hr))
    {
        IAudioData_Release(audio_data);
        return hr;
    }

    TRACE("FUCK2\n");

    IAudioData_Release(audio_data);

    TRACE("FUCK3\n");

    hr = audiomediastream_queue_sample(This->parent, &This->queue_entry);
    TRACE("FUCK4 0x%x\n", hr);
    if (hr != S_OK)
        return hr;

    TRACE("FUCK5\n");

    WaitForSingleObject(This->queue_entry.update_complete_event, INFINITE);

    TRACE("FUCK6 0x%x\n", This->queue_entry.update_result);

    return This->queue_entry.update_result;
}

static HRESULT WINAPI IAudioStreamSampleImpl_CompletionStatus(IAudioStreamSample *iface, DWORD flags, DWORD milliseconds)
{
    FIXME("(%p)->(%x,%u): stub\n", iface, flags, milliseconds);

    return E_NOTIMPL;
}

/*** IAudioStreamSample methods ***/
static HRESULT WINAPI IAudioStreamSampleImpl_GetAudioData(IAudioStreamSample *iface, IAudioData **audio_data)
{
    IAudioStreamSampleImpl *This = impl_from_IAudioStreamSample(iface);

    TRACE("(%p)->(%p)\n", iface, audio_data);

    if (!audio_data)
        return E_POINTER;

    *audio_data = This->audio_data;
    if (*audio_data)
        IAudioData_AddRef(*audio_data);

    return S_OK;
}

static const struct IAudioStreamSampleVtbl AudioStreamSample_Vtbl =
{
    /*** IUnknown methods ***/
    IAudioStreamSampleImpl_QueryInterface,
    IAudioStreamSampleImpl_AddRef,
    IAudioStreamSampleImpl_Release,
    /*** IStreamSample methods ***/
    IAudioStreamSampleImpl_GetMediaStream,
    IAudioStreamSampleImpl_GetSampleTimes,
    IAudioStreamSampleImpl_SetSampleTimes,
    IAudioStreamSampleImpl_Update,
    IAudioStreamSampleImpl_CompletionStatus,
    /*** IAudioStreamSample methods ***/
    IAudioStreamSampleImpl_GetAudioData
};

static HRESULT audiostreamsample_create(IAudioMediaStream *parent, IAudioData *audio_data, IAudioStreamSample **audio_stream_sample)
{
    IAudioStreamSampleImpl *object;
    WAVEFORMATEX format;
    HRESULT hr;

    TRACE("(%p)\n", audio_stream_sample);

    hr = IAudioData_GetFormat(audio_data, &format);
    if (FAILED(hr))
        return hr;

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(IAudioStreamSampleImpl));
    if (!object)
        return E_OUTOFMEMORY;

    object->IAudioStreamSample_iface.lpVtbl = &AudioStreamSample_Vtbl;
    object->ref = 1;
    object->parent = parent;
    object->audio_data = audio_data;

    IAudioData_AddRef(object->audio_data);

    hr = IAudioMediaStream_SetFormat(parent, &format);
    if (FAILED(hr))
        IAudioStreamSample_Release(&object->IAudioStreamSample_iface);

    object->queue_entry.sample = &object->IAudioStreamSample_iface;
    object->queue_entry.update_complete_event = CreateEventW(NULL, FALSE, FALSE, NULL);

    *audio_stream_sample = (IAudioStreamSample*)&object->IAudioStreamSample_iface;

    return S_OK;
}
