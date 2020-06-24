/*
 * Primary DirectDraw video stream
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
#define COBJMACROS
#include "amstream_private.h"
#include "wine/debug.h"
#include "wine/strmbase.h"

WINE_DEFAULT_DEBUG_CHANNEL(amstream);

static const WCHAR sink_id[] = L"I{A35FF56A-9FDA-11D0-8FDF-00C04FD9189D}";

struct queued_receive
{
    struct list entry;
    IMediaSample *sample;
    int stride;
    BYTE *pointer;
};

struct ddraw_stream
{
    IAMMediaStream IAMMediaStream_iface;
    IDirectDrawMediaStream IDirectDrawMediaStream_iface;
    IMemInputPin IMemInputPin_iface;
    IPin IPin_iface;
    LONG ref;
    LONG sample_refs;

    IMultiMediaStream* parent;
    MSPID purpose_id;
    STREAM_TYPE stream_type;
    IDirectDraw *ddraw;
    CRITICAL_SECTION cs;
    IMediaStreamFilter *filter;
    IFilterGraph *graph;

    IPin *peer;
    IMemAllocator *allocator;
    AM_MEDIA_TYPE mt;
    DDSURFACEDESC format;
    FILTER_STATE state;
    BOOL eos;
    struct list receive_queue;
    struct list update_queue;
};

static HRESULT ddrawstreamsample_create(struct ddraw_stream *parent, IDirectDrawSurface *surface,
    const RECT *rect, IDirectDrawStreamSample **ddraw_stream_sample);

static void remove_queued_receive(struct queued_receive *receive)
{
    list_remove(&receive->entry);
    IMediaSample_Release(receive->sample);
    free(receive);
}

static void flush_receive_queue(struct ddraw_stream *stream)
{
    struct list *entry;

    while ((entry = list_head(&stream->receive_queue)))
        remove_queued_receive(LIST_ENTRY(entry, struct queued_receive, entry));
}

static void process_updates(struct ddraw_stream *stream);

static BOOL is_media_type_compatible(const AM_MEDIA_TYPE *media_type, const DDSURFACEDESC *format)
{
    const VIDEOINFOHEADER *video_info = (const VIDEOINFOHEADER *)media_type->pbFormat;

    if ((format->dwFlags & DDSD_WIDTH) && video_info->bmiHeader.biWidth != format->dwWidth)
        return FALSE;

    if ((format->dwFlags & DDSD_HEIGHT) && abs(video_info->bmiHeader.biHeight) != format->dwHeight)
        return FALSE;

    if (format->dwFlags & DDSD_PIXELFORMAT)
    {
        const GUID *subtype = &GUID_NULL;
        switch (format->ddpfPixelFormat.u1.dwRGBBitCount)
        {
        case 8:
            subtype = &MEDIASUBTYPE_RGB8;
            break;
        case 16:
            if (format->ddpfPixelFormat.u3.dwGBitMask == 0x7e0)
                subtype = &MEDIASUBTYPE_RGB565;
            else
                subtype = &MEDIASUBTYPE_RGB555;
            break;
        case 24:
            subtype = &MEDIASUBTYPE_RGB24;
            break;
        case 32:
            subtype = &MEDIASUBTYPE_RGB32;
            break;
        }
        if (!IsEqualGUID(&media_type->subtype, subtype))
            return FALSE;
    }

    return TRUE;
}

static inline struct ddraw_stream *impl_from_IAMMediaStream(IAMMediaStream *iface)
{
    return CONTAINING_RECORD(iface, struct ddraw_stream, IAMMediaStream_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI ddraw_IAMMediaStream_QueryInterface(IAMMediaStream *iface,
        REFIID riid, void **ret_iface)
{
    struct ddraw_stream *This = impl_from_IAMMediaStream(iface);

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
        *ret_iface = &This->IPin_iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IMemInputPin))
    {
        IAMMediaStream_AddRef(iface);
        *ret_iface = &This->IMemInputPin_iface;
        return S_OK;
    }

    ERR("(%p)->(%s,%p),not found\n", This, debugstr_guid(riid), ret_iface);
    return E_NOINTERFACE;
}

static ULONG WINAPI ddraw_IAMMediaStream_AddRef(IAMMediaStream *iface)
{
    struct ddraw_stream *This = impl_from_IAMMediaStream(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p/%p)->(): new ref = %u\n", iface, This, ref);

    return ref;
}

static ULONG WINAPI ddraw_IAMMediaStream_Release(IAMMediaStream *iface)
{
    struct ddraw_stream *stream = impl_from_IAMMediaStream(iface);
    ULONG ref = InterlockedDecrement(&stream->ref);

    TRACE("%p decreasing refcount to %u.\n", stream, ref);

    if (!ref)
    {
        DeleteCriticalSection(&stream->cs);
        if (stream->ddraw)
            IDirectDraw_Release(stream->ddraw);
        HeapFree(GetProcessHeap(), 0, stream);
    }

    return ref;
}

/*** IMediaStream methods ***/
static HRESULT WINAPI ddraw_IAMMediaStream_GetMultiMediaStream(IAMMediaStream *iface,
        IMultiMediaStream **mmstream)
{
    struct ddraw_stream *stream = impl_from_IAMMediaStream(iface);

    TRACE("stream %p, mmstream %p.\n", stream, mmstream);

    if (!mmstream)
        return E_POINTER;

    if (stream->parent)
        IMultiMediaStream_AddRef(stream->parent);
    *mmstream = stream->parent;
    return S_OK;
}

static HRESULT WINAPI ddraw_IAMMediaStream_GetInformation(IAMMediaStream *iface,
        MSPID *purpose_id, STREAM_TYPE *type)
{
    struct ddraw_stream *This = impl_from_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%p,%p)\n", This, iface, purpose_id, type);

    if (purpose_id)
        *purpose_id = This->purpose_id;
    if (type)
        *type = This->stream_type;

    return S_OK;
}

static HRESULT WINAPI ddraw_IAMMediaStream_SetSameFormat(IAMMediaStream *iface,
        IMediaStream *pStreamThatHasDesiredFormat, DWORD flags)
{
    struct ddraw_stream *This = impl_from_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%p,%x) stub!\n", This, iface, pStreamThatHasDesiredFormat, flags);

    return S_FALSE;
}

static HRESULT WINAPI ddraw_IAMMediaStream_AllocateSample(IAMMediaStream *iface,
        DWORD flags, IStreamSample **sample)
{
    struct ddraw_stream *This = impl_from_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%x,%p) stub!\n", This, iface, flags, sample);

    return S_FALSE;
}

static HRESULT WINAPI ddraw_IAMMediaStream_CreateSharedSample(IAMMediaStream *iface,
        IStreamSample *existing_sample, DWORD flags, IStreamSample **sample)
{
    struct ddraw_stream *This = impl_from_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%p,%x,%p) stub!\n", This, iface, existing_sample, flags, sample);

    return S_FALSE;
}

static HRESULT WINAPI ddraw_IAMMediaStream_SendEndOfStream(IAMMediaStream *iface, DWORD flags)
{
    struct ddraw_stream *This = impl_from_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%x) stub!\n", This, iface, flags);

    return S_FALSE;
}

/*** IAMMediaStream methods ***/
static HRESULT WINAPI ddraw_IAMMediaStream_Initialize(IAMMediaStream *iface, IUnknown *source_object, DWORD flags,
                                                    REFMSPID purpose_id, const STREAM_TYPE stream_type)
{
    struct ddraw_stream *stream = impl_from_IAMMediaStream(iface);
    HRESULT hr;

    TRACE("stream %p, source_object %p, flags %x, purpose_id %s, stream_type %u.\n", stream, source_object, flags,
            debugstr_guid(purpose_id), stream_type);

    if (!purpose_id)
        return E_POINTER;

    if (flags & AMMSF_CREATEPEER)
        FIXME("AMMSF_CREATEPEER is not yet supported.\n");

    stream->purpose_id = *purpose_id;
    stream->stream_type = stream_type;

    if (source_object
            && FAILED(hr = IUnknown_QueryInterface(source_object, &IID_IDirectDraw, (void **)&stream->ddraw)))
        FIXME("Stream object doesn't implement IDirectDraw interface, hr %#x.\n", hr);

    if (!source_object)
    {
        if (FAILED(hr = DirectDrawCreate(NULL, &stream->ddraw, NULL)))
            return hr;
        IDirectDraw_SetCooperativeLevel(stream->ddraw, NULL, DDSCL_NORMAL);
    }

    return S_OK;
}

static HRESULT WINAPI ddraw_IAMMediaStream_SetState(IAMMediaStream *iface, FILTER_STATE state)
{
    struct ddraw_stream *stream = impl_from_IAMMediaStream(iface);

    TRACE("stream %p, state %u.\n", stream, state);

    EnterCriticalSection(&stream->cs);

    if (state == State_Stopped)
        flush_receive_queue(stream);
    if (stream->state == State_Stopped)
        stream->eos = FALSE;

    stream->state = state;

    LeaveCriticalSection(&stream->cs);

    return S_OK;
}

static HRESULT WINAPI ddraw_IAMMediaStream_JoinAMMultiMediaStream(IAMMediaStream *iface, IAMMultiMediaStream *mmstream)
{
    struct ddraw_stream *stream = impl_from_IAMMediaStream(iface);

    TRACE("stream %p, mmstream %p.\n", stream, mmstream);

    stream->parent = (IMultiMediaStream *)mmstream;

    return S_OK;
}

static HRESULT WINAPI ddraw_IAMMediaStream_JoinFilter(IAMMediaStream *iface, IMediaStreamFilter *filter)
{
    struct ddraw_stream *stream = impl_from_IAMMediaStream(iface);

    TRACE("iface %p, filter %p.\n", iface, filter);

    stream->filter = filter;

    return S_OK;
}

static HRESULT WINAPI ddraw_IAMMediaStream_JoinFilterGraph(IAMMediaStream *iface, IFilterGraph *filtergraph)
{
    struct ddraw_stream *stream = impl_from_IAMMediaStream(iface);

    TRACE("stream %p, filtergraph %p.\n", stream, filtergraph);

    stream->graph = filtergraph;

    return S_OK;
}

static const struct IAMMediaStreamVtbl ddraw_IAMMediaStream_vtbl =
{
    /*** IUnknown methods ***/
    ddraw_IAMMediaStream_QueryInterface,
    ddraw_IAMMediaStream_AddRef,
    ddraw_IAMMediaStream_Release,
    /*** IMediaStream methods ***/
    ddraw_IAMMediaStream_GetMultiMediaStream,
    ddraw_IAMMediaStream_GetInformation,
    ddraw_IAMMediaStream_SetSameFormat,
    ddraw_IAMMediaStream_AllocateSample,
    ddraw_IAMMediaStream_CreateSharedSample,
    ddraw_IAMMediaStream_SendEndOfStream,
    /*** IAMMediaStream methods ***/
    ddraw_IAMMediaStream_Initialize,
    ddraw_IAMMediaStream_SetState,
    ddraw_IAMMediaStream_JoinAMMultiMediaStream,
    ddraw_IAMMediaStream_JoinFilter,
    ddraw_IAMMediaStream_JoinFilterGraph
};

static inline struct ddraw_stream *impl_from_IDirectDrawMediaStream(IDirectDrawMediaStream *iface)
{
    return CONTAINING_RECORD(iface, struct ddraw_stream, IDirectDrawMediaStream_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI ddraw_IDirectDrawMediaStream_QueryInterface(IDirectDrawMediaStream *iface,
        REFIID riid, void **ret_iface)
{
    struct ddraw_stream *This = impl_from_IDirectDrawMediaStream(iface);
    TRACE("(%p/%p)->(%s,%p)\n", iface, This, debugstr_guid(riid), ret_iface);
    return IAMMediaStream_QueryInterface(&This->IAMMediaStream_iface, riid, ret_iface);
}

static ULONG WINAPI ddraw_IDirectDrawMediaStream_AddRef(IDirectDrawMediaStream *iface)
{
    struct ddraw_stream *This = impl_from_IDirectDrawMediaStream(iface);
    TRACE("(%p/%p)\n", iface, This);
    return IAMMediaStream_AddRef(&This->IAMMediaStream_iface);
}

static ULONG WINAPI ddraw_IDirectDrawMediaStream_Release(IDirectDrawMediaStream *iface)
{
    struct ddraw_stream *This = impl_from_IDirectDrawMediaStream(iface);
    TRACE("(%p/%p)\n", iface, This);
    return IAMMediaStream_Release(&This->IAMMediaStream_iface);
}

static HRESULT WINAPI ddraw_IDirectDrawMediaStream_GetMultiMediaStream(IDirectDrawMediaStream *iface,
        IMultiMediaStream **mmstream)
{
    struct ddraw_stream *stream = impl_from_IDirectDrawMediaStream(iface);
    return IAMMediaStream_GetMultiMediaStream(&stream->IAMMediaStream_iface, mmstream);
}

static HRESULT WINAPI ddraw_IDirectDrawMediaStream_GetInformation(IDirectDrawMediaStream *iface,
        MSPID *purpose_id, STREAM_TYPE *type)
{
    struct ddraw_stream *stream = impl_from_IDirectDrawMediaStream(iface);
    return IAMMediaStream_GetInformation(&stream->IAMMediaStream_iface, purpose_id, type);
}

static HRESULT WINAPI ddraw_IDirectDrawMediaStream_SetSameFormat(IDirectDrawMediaStream *iface,
        IMediaStream *other, DWORD flags)
{
    struct ddraw_stream *stream = impl_from_IDirectDrawMediaStream(iface);
    return IAMMediaStream_SetSameFormat(&stream->IAMMediaStream_iface, other, flags);
}

static HRESULT WINAPI ddraw_IDirectDrawMediaStream_AllocateSample(IDirectDrawMediaStream *iface,
        DWORD flags, IStreamSample **sample)
{
    struct ddraw_stream *stream = impl_from_IDirectDrawMediaStream(iface);
    return IAMMediaStream_AllocateSample(&stream->IAMMediaStream_iface, flags, sample);
}

static HRESULT WINAPI ddraw_IDirectDrawMediaStream_CreateSharedSample(IDirectDrawMediaStream *iface,
        IStreamSample *existing_sample, DWORD flags, IStreamSample **sample)
{
    struct ddraw_stream *stream = impl_from_IDirectDrawMediaStream(iface);
    return IAMMediaStream_CreateSharedSample(&stream->IAMMediaStream_iface, existing_sample, flags, sample);
}

static HRESULT WINAPI ddraw_IDirectDrawMediaStream_SendEndOfStream(IDirectDrawMediaStream *iface, DWORD flags)
{
    struct ddraw_stream *stream = impl_from_IDirectDrawMediaStream(iface);
    return IAMMediaStream_SendEndOfStream(&stream->IAMMediaStream_iface, flags);
}

/*** IDirectDrawMediaStream methods ***/
static HRESULT WINAPI ddraw_IDirectDrawMediaStream_GetFormat(IDirectDrawMediaStream *iface,
        DDSURFACEDESC *current_format, IDirectDrawPalette **palette,
        DDSURFACEDESC *desired_format, DWORD *flags)
{
    struct ddraw_stream *stream = impl_from_IDirectDrawMediaStream(iface);
    VIDEOINFOHEADER *video_info;

    TRACE("stream %p, current_format %p, palette %p, desired_format %p, flags %p.\n", stream, current_format, palette,
            desired_format, flags);

    EnterCriticalSection(&stream->cs);

    if (!stream->peer)
    {
        LeaveCriticalSection(&stream->cs);
        return MS_E_NOSTREAM;
    }

    video_info = (VIDEOINFOHEADER *)stream->mt.pbFormat;

    if (current_format)
    {
        current_format->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS | (stream->format.dwFlags & DDSD_PIXELFORMAT);
        current_format->dwWidth = video_info->bmiHeader.biWidth;
        current_format->dwHeight = abs(video_info->bmiHeader.biHeight);
        current_format->ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        current_format->ddpfPixelFormat = stream->format.ddpfPixelFormat;
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

    LeaveCriticalSection(&stream->cs);

    return S_OK;
}

static HRESULT WINAPI ddraw_IDirectDrawMediaStream_SetFormat(IDirectDrawMediaStream *iface,
        const DDSURFACEDESC *format, IDirectDrawPalette *palette)
{
    struct ddraw_stream *stream = impl_from_IDirectDrawMediaStream(iface);
    IGraphBuilder *graph_builder;
    AM_MEDIA_TYPE old_media_type;
    DDSURFACEDESC old_format;
    IPin *old_peer;
    HRESULT hr;

    TRACE("stream %p, format %p, palette %p.\n", stream, format, palette);

    if (!format)
        return E_POINTER;

    if (format->dwSize != sizeof(DDSURFACEDESC))
        return E_INVALIDARG;

    if (format->dwFlags & DDSD_PIXELFORMAT)
    {
        if (format->ddpfPixelFormat.dwSize != sizeof(DDPIXELFORMAT))
            return DDERR_INVALIDSURFACETYPE;

        if (format->ddpfPixelFormat.dwFlags & (DDPF_YUV | DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 | DDPF_PALETTEINDEXED4 | DDPF_PALETTEINDEXEDTO8))
            return DDERR_INVALIDSURFACETYPE;

        if (!(format->ddpfPixelFormat.dwFlags & DDPF_RGB))
            return DDERR_INVALIDSURFACETYPE;

        switch (format->ddpfPixelFormat.u1.dwRGBBitCount)
        {
        case 8:
            if (!(format->ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8))
                return DDERR_INVALIDSURFACETYPE;
            break;
        case 16:
            if (format->ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8)
                return DDERR_INVALIDSURFACETYPE;
            if ((format->ddpfPixelFormat.u2.dwRBitMask != 0x7c00 ||
                format->ddpfPixelFormat.u3.dwGBitMask != 0x03e0 ||
                format->ddpfPixelFormat.u4.dwBBitMask != 0x001f) &&
                (format->ddpfPixelFormat.u2.dwRBitMask != 0xf800 ||
                format->ddpfPixelFormat.u3.dwGBitMask != 0x07e0 ||
                format->ddpfPixelFormat.u4.dwBBitMask != 0x001f))
                return DDERR_INVALIDSURFACETYPE;
            break;
        case 24:
        case 32:
            if (format->ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8)
                return DDERR_INVALIDSURFACETYPE;
            if (format->ddpfPixelFormat.u2.dwRBitMask != 0xff0000 ||
                format->ddpfPixelFormat.u3.dwGBitMask != 0x00ff00 ||
                format->ddpfPixelFormat.u4.dwBBitMask != 0x0000ff)
                return DDERR_INVALIDSURFACETYPE;
            break;
        default:
            return DDERR_INVALIDSURFACETYPE;
        }
    }

    EnterCriticalSection(&stream->cs);

    old_format = stream->format;
    stream->format = *format;

    if (stream->peer && !is_media_type_compatible(&stream->mt, &stream->format))
    {
        hr = IFilterGraph_QueryInterface(stream->graph, &IID_IGraphBuilder, (void *)&graph_builder);
        if (FAILED(hr))
        {
            stream->format = old_format;
            LeaveCriticalSection(&stream->cs);
            return hr;
        }
        hr = CopyMediaType(&old_media_type, &stream->mt);
        if (FAILED(hr))
        {
            stream->format = old_format;
            IGraphBuilder_Release(graph_builder);
            LeaveCriticalSection(&stream->cs);
            return hr;
        }
        old_peer = stream->peer;
        IPin_AddRef(old_peer);

        IGraphBuilder_Disconnect(graph_builder, stream->peer);
        IGraphBuilder_Disconnect(graph_builder, &stream->IPin_iface);
        hr = IGraphBuilder_Connect(graph_builder, old_peer, &stream->IPin_iface);
        if (FAILED(hr))
        {
            stream->format = old_format;
            IGraphBuilder_ConnectDirect(graph_builder, old_peer, &stream->IPin_iface, &old_media_type);
            IPin_Release(old_peer);
            FreeMediaType(&old_media_type);
            IGraphBuilder_Release(graph_builder);
            LeaveCriticalSection(&stream->cs);
            return DDERR_INVALIDSURFACETYPE;
        }

        IPin_Release(old_peer);
        FreeMediaType(&old_media_type);
        IGraphBuilder_Release(graph_builder);
    }

    LeaveCriticalSection(&stream->cs);

    return S_OK;
}

static HRESULT WINAPI ddraw_IDirectDrawMediaStream_GetDirectDraw(IDirectDrawMediaStream *iface,
        IDirectDraw **ddraw)
{
    struct ddraw_stream *stream = impl_from_IDirectDrawMediaStream(iface);

    TRACE("stream %p, ddraw %p.\n", stream, ddraw);

    if (!ddraw)
        return E_POINTER;

    if (!stream->ddraw)
    {
        *ddraw = NULL;
        return S_OK;
    }

    IDirectDraw_AddRef(stream->ddraw);
    *ddraw = stream->ddraw;

    return S_OK;
}

static HRESULT WINAPI ddraw_IDirectDrawMediaStream_SetDirectDraw(IDirectDrawMediaStream *iface,
        IDirectDraw *ddraw)
{
    struct ddraw_stream *stream = impl_from_IDirectDrawMediaStream(iface);

    TRACE("stream %p, ddraw %p.\n", stream, ddraw);

    EnterCriticalSection(&stream->cs);

    if (stream->sample_refs)
    {
        HRESULT hr = (stream->ddraw == ddraw) ? S_OK : MS_E_SAMPLEALLOC;
        LeaveCriticalSection(&stream->cs);
        return hr;
    }

    if (stream->ddraw)
        IDirectDraw_Release(stream->ddraw);

    if (ddraw)
    {
        IDirectDraw_AddRef(ddraw);
        stream->ddraw = ddraw;
    }
    else
        stream->ddraw = NULL;

    LeaveCriticalSection(&stream->cs);

    return S_OK;
}

static HRESULT WINAPI ddraw_IDirectDrawMediaStream_CreateSample(IDirectDrawMediaStream *iface,
        IDirectDrawSurface *surface, const RECT *rect, DWORD flags,
        IDirectDrawStreamSample **sample)
{
    struct ddraw_stream *stream = impl_from_IDirectDrawMediaStream(iface);
    HRESULT hr;

    TRACE("stream %p, surface %p, rect %s, flags %#x, sample %p.\n",
            stream, surface, wine_dbgstr_rect(rect), flags, sample);

    if (!surface && rect)
        return E_INVALIDARG;

    EnterCriticalSection(&stream->cs);
    hr = ddrawstreamsample_create(stream, surface, rect, sample);
    LeaveCriticalSection(&stream->cs);

    return hr;
}

static HRESULT WINAPI ddraw_IDirectDrawMediaStream_GetTimePerFrame(IDirectDrawMediaStream *iface,
        STREAM_TIME *pFrameTime)
{
    FIXME("(%p)->(%p) stub!\n", iface, pFrameTime);

    return E_NOTIMPL;
}

static const struct IDirectDrawMediaStreamVtbl ddraw_IDirectDrawMediaStream_Vtbl =
{
    /*** IUnknown methods ***/
    ddraw_IDirectDrawMediaStream_QueryInterface,
    ddraw_IDirectDrawMediaStream_AddRef,
    ddraw_IDirectDrawMediaStream_Release,
    /*** IMediaStream methods ***/
    ddraw_IDirectDrawMediaStream_GetMultiMediaStream,
    ddraw_IDirectDrawMediaStream_GetInformation,
    ddraw_IDirectDrawMediaStream_SetSameFormat,
    ddraw_IDirectDrawMediaStream_AllocateSample,
    ddraw_IDirectDrawMediaStream_CreateSharedSample,
    ddraw_IDirectDrawMediaStream_SendEndOfStream,
    /*** IDirectDrawMediaStream methods ***/
    ddraw_IDirectDrawMediaStream_GetFormat,
    ddraw_IDirectDrawMediaStream_SetFormat,
    ddraw_IDirectDrawMediaStream_GetDirectDraw,
    ddraw_IDirectDrawMediaStream_SetDirectDraw,
    ddraw_IDirectDrawMediaStream_CreateSample,
    ddraw_IDirectDrawMediaStream_GetTimePerFrame
};

struct enum_media_types
{
    IEnumMediaTypes IEnumMediaTypes_iface;
    LONG refcount;
    unsigned int index;
};

static const IEnumMediaTypesVtbl enum_media_types_vtbl;

static struct enum_media_types *impl_from_IEnumMediaTypes(IEnumMediaTypes *iface)
{
    return CONTAINING_RECORD(iface, struct enum_media_types, IEnumMediaTypes_iface);
}

static HRESULT WINAPI enum_media_types_QueryInterface(IEnumMediaTypes *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IEnumMediaTypes))
    {
        IEnumMediaTypes_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI enum_media_types_AddRef(IEnumMediaTypes *iface)
{
    struct enum_media_types *enum_media_types = impl_from_IEnumMediaTypes(iface);
    ULONG refcount = InterlockedIncrement(&enum_media_types->refcount);
    TRACE("%p increasing refcount to %u.\n", enum_media_types, refcount);
    return refcount;
}

static ULONG WINAPI enum_media_types_Release(IEnumMediaTypes *iface)
{
    struct enum_media_types *enum_media_types = impl_from_IEnumMediaTypes(iface);
    ULONG refcount = InterlockedDecrement(&enum_media_types->refcount);
    TRACE("%p decreasing refcount to %u.\n", enum_media_types, refcount);
    if (!refcount)
        heap_free(enum_media_types);
    return refcount;
}

static HRESULT WINAPI enum_media_types_Next(IEnumMediaTypes *iface, ULONG count, AM_MEDIA_TYPE **mts, ULONG *ret_count)
{
    struct enum_media_types *enum_media_types = impl_from_IEnumMediaTypes(iface);

    TRACE("iface %p, count %u, mts %p, ret_count %p.\n", iface, count, mts, ret_count);

    if (!ret_count)
        return E_POINTER;

    if (count && !enum_media_types->index)
    {
        mts[0] = CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
        memset(mts[0], 0, sizeof(AM_MEDIA_TYPE));
        mts[0]->majortype = MEDIATYPE_Video;
        mts[0]->subtype = MEDIASUBTYPE_RGB8;
        mts[0]->bFixedSizeSamples = TRUE;
        mts[0]->lSampleSize = 10000;
        ++enum_media_types->index;
        *ret_count = 1;
        return count == 1 ? S_OK : S_FALSE;
    }

    *ret_count = 0;
    return count ? S_FALSE : S_OK;
}

static HRESULT WINAPI enum_media_types_Skip(IEnumMediaTypes *iface, ULONG count)
{
    struct enum_media_types *enum_media_types = impl_from_IEnumMediaTypes(iface);

    TRACE("iface %p, count %u.\n", iface, count);

    enum_media_types->index += count;

    return S_OK;
}

static HRESULT WINAPI enum_media_types_Reset(IEnumMediaTypes *iface)
{
    struct enum_media_types *enum_media_types = impl_from_IEnumMediaTypes(iface);

    TRACE("iface %p.\n", iface);

    enum_media_types->index = 0;
    return S_OK;
}

static HRESULT WINAPI enum_media_types_Clone(IEnumMediaTypes *iface, IEnumMediaTypes **out)
{
    struct enum_media_types *enum_media_types = impl_from_IEnumMediaTypes(iface);
    struct enum_media_types *object;

    TRACE("iface %p, out %p.\n", iface, out);

    if (!(object = heap_alloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IEnumMediaTypes_iface.lpVtbl = &enum_media_types_vtbl;
    object->refcount = 1;
    object->index = enum_media_types->index;

    *out = &object->IEnumMediaTypes_iface;
    return S_OK;
}

static const IEnumMediaTypesVtbl enum_media_types_vtbl =
{
    enum_media_types_QueryInterface,
    enum_media_types_AddRef,
    enum_media_types_Release,
    enum_media_types_Next,
    enum_media_types_Skip,
    enum_media_types_Reset,
    enum_media_types_Clone,
};

static inline struct ddraw_stream *impl_from_IPin(IPin *iface)
{
    return CONTAINING_RECORD(iface, struct ddraw_stream, IPin_iface);
}

static HRESULT WINAPI ddraw_sink_QueryInterface(IPin *iface, REFIID iid, void **out)
{
    struct ddraw_stream *stream = impl_from_IPin(iface);
    return IAMMediaStream_QueryInterface(&stream->IAMMediaStream_iface, iid, out);
}

static ULONG WINAPI ddraw_sink_AddRef(IPin *iface)
{
    struct ddraw_stream *stream = impl_from_IPin(iface);
    return IAMMediaStream_AddRef(&stream->IAMMediaStream_iface);
}

static ULONG WINAPI ddraw_sink_Release(IPin *iface)
{
    struct ddraw_stream *stream = impl_from_IPin(iface);
    return IAMMediaStream_Release(&stream->IAMMediaStream_iface);
}

static HRESULT WINAPI ddraw_sink_Connect(IPin *iface, IPin *peer, const AM_MEDIA_TYPE *mt)
{
    WARN("iface %p, peer %p, mt %p, unexpected call!\n", iface, peer, mt);
    return E_UNEXPECTED;
}

static HRESULT WINAPI ddraw_sink_ReceiveConnection(IPin *iface, IPin *peer, const AM_MEDIA_TYPE *mt)
{
    struct ddraw_stream *stream = impl_from_IPin(iface);
    PIN_DIRECTION dir;

    TRACE("stream %p, peer %p, mt %p.\n", stream, peer, mt);

    EnterCriticalSection(&stream->cs);

    if (stream->peer)
    {
        LeaveCriticalSection(&stream->cs);
        return VFW_E_ALREADY_CONNECTED;
    }

    if (!IsEqualGUID(&mt->majortype, &MEDIATYPE_Video)
            || (!IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB8)
                && !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB24)
                && !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB32)
                && !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB555)
                && !IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB565))
            || !IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo)
            || !is_media_type_compatible(mt, &stream->format))
    {
        LeaveCriticalSection(&stream->cs);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    IPin_QueryDirection(peer, &dir);
    if (dir != PINDIR_OUTPUT)
    {
        WARN("Rejecting connection from input pin.\n");
        LeaveCriticalSection(&stream->cs);
        return VFW_E_INVALID_DIRECTION;
    }

    CopyMediaType(&stream->mt, mt);
    IPin_AddRef(stream->peer = peer);

    LeaveCriticalSection(&stream->cs);

    return S_OK;
}

static HRESULT WINAPI ddraw_sink_Disconnect(IPin *iface)
{
    struct ddraw_stream *stream = impl_from_IPin(iface);

    TRACE("stream %p.\n", stream);

    EnterCriticalSection(&stream->cs);

    if (!stream->peer)
    {
        LeaveCriticalSection(&stream->cs);
        return S_FALSE;
    }

    IPin_Release(stream->peer);
    stream->peer = NULL;
    FreeMediaType(&stream->mt);
    memset(&stream->mt, 0, sizeof(AM_MEDIA_TYPE));

    LeaveCriticalSection(&stream->cs);

    return S_OK;
}

static HRESULT WINAPI ddraw_sink_ConnectedTo(IPin *iface, IPin **peer)
{
    struct ddraw_stream *stream = impl_from_IPin(iface);
    HRESULT hr;

    TRACE("stream %p, peer %p.\n", stream, peer);

    EnterCriticalSection(&stream->cs);

    if (stream->peer)
    {
        IPin_AddRef(*peer = stream->peer);
        hr = S_OK;
    }
    else
    {
        *peer = NULL;
        hr = VFW_E_NOT_CONNECTED;
    }

    LeaveCriticalSection(&stream->cs);

    return hr;
}

static HRESULT WINAPI ddraw_sink_ConnectionMediaType(IPin *iface, AM_MEDIA_TYPE *mt)
{
    struct ddraw_stream *stream = impl_from_IPin(iface);
    HRESULT hr;

    TRACE("stream %p, mt %p.\n", stream, mt);

    EnterCriticalSection(&stream->cs);

    if (stream->peer)
    {
        CopyMediaType(mt, &stream->mt);
        hr = S_OK;
    }
    else
    {
        memset(mt, 0, sizeof(AM_MEDIA_TYPE));
        hr = VFW_E_NOT_CONNECTED;
    }

    LeaveCriticalSection(&stream->cs);

    return hr;
}

static HRESULT WINAPI ddraw_sink_QueryPinInfo(IPin *iface, PIN_INFO *info)
{
    struct ddraw_stream *stream = impl_from_IPin(iface);

    TRACE("stream %p, info %p.\n", stream, info);

    IBaseFilter_AddRef(info->pFilter = (IBaseFilter *)stream->filter);
    info->dir = PINDIR_INPUT;
    wcscpy(info->achName, sink_id);

    return S_OK;
}

static HRESULT WINAPI ddraw_sink_QueryDirection(IPin *iface, PIN_DIRECTION *dir)
{
    TRACE("iface %p, dir %p.\n", iface, dir);
    *dir = PINDIR_INPUT;
    return S_OK;
}

static HRESULT WINAPI ddraw_sink_QueryId(IPin *iface, WCHAR **id)
{
    TRACE("iface %p, id %p.\n", iface, id);

    if (!(*id = CoTaskMemAlloc(sizeof(sink_id))))
        return E_OUTOFMEMORY;

    wcscpy(*id, sink_id);

    return S_OK;
}

static HRESULT WINAPI ddraw_sink_QueryAccept(IPin *iface, const AM_MEDIA_TYPE *mt)
{
    TRACE("iface %p, mt %p.\n", iface, mt);

    if (IsEqualGUID(&mt->majortype, &MEDIATYPE_Video)
            && IsEqualGUID(&mt->subtype, &MEDIASUBTYPE_RGB8)
            && IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo))
        return S_OK;

    return VFW_E_TYPE_NOT_ACCEPTED;
}

static HRESULT WINAPI ddraw_sink_EnumMediaTypes(IPin *iface, IEnumMediaTypes **enum_media_types)
{
    struct enum_media_types *object;

    TRACE("iface %p, enum_media_types %p.\n", iface, enum_media_types);

    if (!enum_media_types)
        return E_POINTER;

    if (!(object = heap_alloc(sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IEnumMediaTypes_iface.lpVtbl = &enum_media_types_vtbl;
    object->refcount = 1;
    object->index = 0;

    *enum_media_types = &object->IEnumMediaTypes_iface;
    return S_OK;
}

static HRESULT WINAPI ddraw_sink_QueryInternalConnections(IPin *iface, IPin **pins, ULONG *count)
{
    TRACE("iface %p, pins %p, count %p.\n", iface, pins, count);
    return E_NOTIMPL;
}

static HRESULT WINAPI ddraw_sink_EndOfStream(IPin *iface)
{
    struct ddraw_stream *stream = impl_from_IPin(iface);

    TRACE("stream %p.\n", stream);

    EnterCriticalSection(&stream->cs);

    if (stream->eos)
    {
        LeaveCriticalSection(&stream->cs);
        return E_FAIL;
    }

    stream->eos = TRUE;

    process_updates(stream);

    LeaveCriticalSection(&stream->cs);

    return S_OK;
}

static HRESULT WINAPI ddraw_sink_BeginFlush(IPin *iface)
{
    FIXME("iface %p, stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI ddraw_sink_EndFlush(IPin *iface)
{
    FIXME("iface %p, stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI ddraw_sink_NewSegment(IPin *iface, REFERENCE_TIME start, REFERENCE_TIME stop, double rate)
{
    FIXME("iface %p, start %s, stop %s, rate %0.16e, stub!\n",
            iface, wine_dbgstr_longlong(start), wine_dbgstr_longlong(stop), rate);
    return E_NOTIMPL;
}

static const IPinVtbl ddraw_sink_vtbl =
{
    ddraw_sink_QueryInterface,
    ddraw_sink_AddRef,
    ddraw_sink_Release,
    ddraw_sink_Connect,
    ddraw_sink_ReceiveConnection,
    ddraw_sink_Disconnect,
    ddraw_sink_ConnectedTo,
    ddraw_sink_ConnectionMediaType,
    ddraw_sink_QueryPinInfo,
    ddraw_sink_QueryDirection,
    ddraw_sink_QueryId,
    ddraw_sink_QueryAccept,
    ddraw_sink_EnumMediaTypes,
    ddraw_sink_QueryInternalConnections,
    ddraw_sink_EndOfStream,
    ddraw_sink_BeginFlush,
    ddraw_sink_EndFlush,
    ddraw_sink_NewSegment,
};

static inline struct ddraw_stream *impl_from_IMemInputPin(IMemInputPin *iface)
{
    return CONTAINING_RECORD(iface, struct ddraw_stream, IMemInputPin_iface);
}

static HRESULT WINAPI ddraw_meminput_QueryInterface(IMemInputPin *iface, REFIID iid, void **out)
{
    struct ddraw_stream *stream = impl_from_IMemInputPin(iface);
    return IAMMediaStream_QueryInterface(&stream->IAMMediaStream_iface, iid, out);
}

static ULONG WINAPI ddraw_meminput_AddRef(IMemInputPin *iface)
{
    struct ddraw_stream *stream = impl_from_IMemInputPin(iface);
    return IAMMediaStream_AddRef(&stream->IAMMediaStream_iface);
}

static ULONG WINAPI ddraw_meminput_Release(IMemInputPin *iface)
{
    struct ddraw_stream *stream = impl_from_IMemInputPin(iface);
    return IAMMediaStream_Release(&stream->IAMMediaStream_iface);
}

static HRESULT WINAPI ddraw_meminput_GetAllocator(IMemInputPin *iface, IMemAllocator **allocator)
{
    struct ddraw_stream *stream = impl_from_IMemInputPin(iface);

    TRACE("stream %p, allocator %p.\n", stream, allocator);

    if (stream->allocator)
    {
        IMemAllocator_AddRef(*allocator = stream->allocator);
        return S_OK;
    }

    *allocator = NULL;
    return VFW_E_NO_ALLOCATOR;
}

static HRESULT WINAPI ddraw_meminput_NotifyAllocator(IMemInputPin *iface, IMemAllocator *allocator, BOOL readonly)
{
    struct ddraw_stream *stream = impl_from_IMemInputPin(iface);

    TRACE("stream %p, allocator %p, readonly %d.\n", stream, allocator, readonly);

    if (!allocator)
        return E_POINTER;

    if (allocator)
        IMemAllocator_AddRef(allocator);
    if (stream->allocator)
        IMemAllocator_Release(stream->allocator);
    stream->allocator = allocator;

    return S_OK;
}

static HRESULT WINAPI ddraw_meminput_GetAllocatorRequirements(IMemInputPin *iface, ALLOCATOR_PROPERTIES *props)
{
    TRACE("iface %p, props %p.\n", iface, props);
    return E_NOTIMPL;
}

static HRESULT WINAPI ddraw_meminput_Receive(IMemInputPin *iface, IMediaSample *sample)
{
    struct ddraw_stream *stream = impl_from_IMemInputPin(iface);
    struct queued_receive *receive;
    BITMAPINFOHEADER *bitmap_info = &((VIDEOINFOHEADER *)stream->mt.pbFormat)->bmiHeader;
    BYTE *pointer;
    int stride;
    HRESULT hr;

    TRACE("stream %p, sample %p.\n", stream, sample);

    EnterCriticalSection(&stream->cs);

    if (stream->state == State_Stopped)
    {
        LeaveCriticalSection(&stream->cs);
        return VFW_E_WRONG_STATE;
    }

    hr = IMediaSample_GetPointer(sample, &pointer);
    if (FAILED(hr))
    {
        LeaveCriticalSection(&stream->cs);
        return hr;
    }

    receive = calloc(1, sizeof(*receive));
    if (!receive)
    {
        LeaveCriticalSection(&stream->cs);
        return E_OUTOFMEMORY;
    }

    stride = ((bitmap_info->biWidth * bitmap_info->biBitCount + 31) & ~31) >> 3;

    receive->stride = (bitmap_info->biHeight > 0) ? -stride : stride;
    receive->pointer = (bitmap_info->biHeight > 0) ? pointer + stride * (bitmap_info->biHeight - 1) : pointer;
    receive->sample = sample;
    IMediaSample_AddRef(receive->sample);
    list_add_tail(&stream->receive_queue, &receive->entry);

    process_updates(stream);

    LeaveCriticalSection(&stream->cs);

    return S_OK;
}

static HRESULT WINAPI ddraw_meminput_ReceiveMultiple(IMemInputPin *iface,
        IMediaSample **samples, LONG count, LONG *processed)
{
    FIXME("iface %p, samples %p, count %u, processed %p, stub!\n", iface, samples, count, processed);
    return E_NOTIMPL;
}

static HRESULT WINAPI ddraw_meminput_ReceiveCanBlock(IMemInputPin *iface)
{
    TRACE("iface %p.\n", iface);
    return S_OK;
}

static const IMemInputPinVtbl ddraw_meminput_vtbl =
{
    ddraw_meminput_QueryInterface,
    ddraw_meminput_AddRef,
    ddraw_meminput_Release,
    ddraw_meminput_GetAllocator,
    ddraw_meminput_NotifyAllocator,
    ddraw_meminput_GetAllocatorRequirements,
    ddraw_meminput_Receive,
    ddraw_meminput_ReceiveMultiple,
    ddraw_meminput_ReceiveCanBlock,
};

HRESULT ddraw_stream_create(IUnknown *outer, void **out)
{
    struct ddraw_stream *object;

    if (outer)
        return CLASS_E_NOAGGREGATION;

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if (!object)
        return E_OUTOFMEMORY;

    object->IAMMediaStream_iface.lpVtbl = &ddraw_IAMMediaStream_vtbl;
    object->IDirectDrawMediaStream_iface.lpVtbl = &ddraw_IDirectDrawMediaStream_Vtbl;
    object->IMemInputPin_iface.lpVtbl = &ddraw_meminput_vtbl;
    object->IPin_iface.lpVtbl = &ddraw_sink_vtbl;
    object->ref = 1;

    InitializeCriticalSection(&object->cs);
    list_init(&object->receive_queue);
    list_init(&object->update_queue);

    TRACE("Created ddraw stream %p.\n", object);

    *out = &object->IAMMediaStream_iface;

    return S_OK;
}

struct ddraw_sample
{
    IDirectDrawStreamSample IDirectDrawStreamSample_iface;
    LONG ref;
    struct ddraw_stream *parent;
    IDirectDrawSurface *surface;
    RECT rect;
    HANDLE update_event;

    struct list entry;
    DWORD stride;
    DWORD row_size;
    BYTE *pointer;
    HRESULT update_hr;
};

static void remove_queued_update(struct ddraw_sample *sample)
{
    HRESULT hr;

    hr = IDirectDrawSurface_Unlock(sample->surface, sample->pointer);
    if (FAILED(hr))
        sample->update_hr = hr;

    list_remove(&sample->entry);
    SetEvent(sample->update_event);
}

static void process_update(struct ddraw_sample *sample, struct queued_receive *receive)
{
    const BYTE *src_row = receive->pointer;
    BYTE *dst_row = sample->pointer;
    DWORD row;

    for (row = sample->rect.top; row < sample->rect.bottom; ++row)
    {
        memcpy(dst_row, src_row, sample->row_size);
        src_row += receive->stride;
        dst_row += sample->stride;
    }

    sample->update_hr = S_OK;
}

static void process_updates(struct ddraw_stream *stream)
{
    while (!list_empty(&stream->update_queue) && !list_empty(&stream->receive_queue))
    {
        struct ddraw_sample *sample = LIST_ENTRY(list_head(&stream->update_queue), struct ddraw_sample, entry);
        struct queued_receive *receive = LIST_ENTRY(list_head(&stream->receive_queue), struct queued_receive, entry);

        process_update(sample, receive);

        remove_queued_update(sample);
        remove_queued_receive(receive);
    }
    if (stream->eos)
    {
        while (!list_empty(&stream->update_queue))
        {
            struct ddraw_sample *sample = LIST_ENTRY(list_head(&stream->update_queue), struct ddraw_sample, entry);

            sample->update_hr = MS_S_ENDOFSTREAM;
            remove_queued_update(sample);
        }
    }
}

static inline struct ddraw_sample *impl_from_IDirectDrawStreamSample(IDirectDrawStreamSample *iface)
{
    return CONTAINING_RECORD(iface, struct ddraw_sample, IDirectDrawStreamSample_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI ddraw_sample_QueryInterface(IDirectDrawStreamSample *iface,
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

static ULONG WINAPI ddraw_sample_AddRef(IDirectDrawStreamSample *iface)
{
    struct ddraw_sample *sample = impl_from_IDirectDrawStreamSample(iface);
    ULONG ref = InterlockedIncrement(&sample->ref);

    TRACE("(%p)->(): new ref = %u\n", iface, ref);

    return ref;
}

static ULONG WINAPI ddraw_sample_Release(IDirectDrawStreamSample *iface)
{
    struct ddraw_sample *sample = impl_from_IDirectDrawStreamSample(iface);
    ULONG ref = InterlockedDecrement(&sample->ref);

    TRACE("(%p)->(): new ref = %u\n", iface, ref);

    if (!ref)
    {
        EnterCriticalSection(&sample->parent->cs);
        --sample->parent->sample_refs;
        LeaveCriticalSection(&sample->parent->cs);

        IAMMediaStream_Release(&sample->parent->IAMMediaStream_iface);

        if (sample->surface)
            IDirectDrawSurface_Release(sample->surface);
        CloseHandle(sample->update_event);
        HeapFree(GetProcessHeap(), 0, sample);
    }

    return ref;
}

/*** IStreamSample methods ***/
static HRESULT WINAPI ddraw_sample_GetMediaStream(IDirectDrawStreamSample *iface, IMediaStream **media_stream)
{
    struct ddraw_sample *sample = impl_from_IDirectDrawStreamSample(iface);

    TRACE("sample %p, media_stream %p.\n", sample, media_stream);

    if (!media_stream)
        return E_POINTER;

    IAMMediaStream_AddRef(&sample->parent->IAMMediaStream_iface);
    *media_stream = (IMediaStream *)&sample->parent->IAMMediaStream_iface;

    return S_OK;
}

static HRESULT WINAPI ddraw_sample_GetSampleTimes(IDirectDrawStreamSample *iface, STREAM_TIME *start_time,
                                                                 STREAM_TIME *end_time, STREAM_TIME *current_time)
{
    FIXME("(%p)->(%p,%p,%p): stub\n", iface, start_time, end_time, current_time);

    return E_NOTIMPL;
}

static HRESULT WINAPI ddraw_sample_SetSampleTimes(IDirectDrawStreamSample *iface, const STREAM_TIME *start_time,
                                                                 const STREAM_TIME *end_time)
{
    FIXME("(%p)->(%p,%p): stub\n", iface, start_time, end_time);

    return E_NOTIMPL;
}

static HRESULT WINAPI ddraw_sample_Update(IDirectDrawStreamSample *iface,
        DWORD flags, HANDLE event, PAPCFUNC apc_func, DWORD apc_data)
{
    struct ddraw_sample *sample = impl_from_IDirectDrawStreamSample(iface);
    DDSURFACEDESC desc = { sizeof(DDSURFACEDESC) };
    HRESULT hr;

    TRACE("sample %p, flags %#x, event %p, apc_func %p, apc_data %#x.\n",
            sample, flags, event, apc_func, apc_data);

    if (event && apc_func)
        return E_INVALIDARG;

    if (apc_func)
    {
        FIXME("APC support is not implemented!\n");
        return E_NOTIMPL;
    }

    if (event)
    {
        FIXME("Event parameter support is not implemented!\n");
        return E_NOTIMPL;
    }

    if (flags & ~SSUPDATE_ASYNC)
    {
        FIXME("Unsupported flags %#x.\n", flags);
        return E_NOTIMPL;
    }

    EnterCriticalSection(&sample->parent->cs);

    if (sample->parent->state != State_Running)
    {
        LeaveCriticalSection(&sample->parent->cs);
        return MS_E_NOTRUNNING;
    }
    if (!sample->parent->peer)
    {
        LeaveCriticalSection(&sample->parent->cs);
        return MS_S_ENDOFSTREAM;
    }
    if (MS_S_PENDING == sample->update_hr)
    {
        LeaveCriticalSection(&sample->parent->cs);
        return MS_E_BUSY;
    }

    hr = IDirectDrawSurface_Lock(sample->surface, &sample->rect, &desc, DDLOCK_WAIT, NULL);
    if (FAILED(hr))
    {
        LeaveCriticalSection(&sample->parent->cs);
        return hr;
    }

    sample->stride = desc.u1.lPitch;
    sample->row_size = (sample->rect.right - sample->rect.left) * desc.ddpfPixelFormat.u1.dwRGBBitCount >> 3;
    sample->pointer = desc.lpSurface;
    sample->update_hr = MS_S_PENDING;
    ResetEvent(sample->update_event);
    list_add_tail(&sample->parent->update_queue, &sample->entry);

    process_updates(sample->parent);
    hr = sample->update_hr;

    LeaveCriticalSection(&sample->parent->cs);

    if (hr != MS_S_PENDING || (flags & SSUPDATE_ASYNC))
        return hr;

    WaitForSingleObject(sample->update_event, INFINITE);

    return sample->update_hr;
}

static HRESULT WINAPI ddraw_sample_CompletionStatus(IDirectDrawStreamSample *iface, DWORD flags, DWORD milliseconds)
{
    FIXME("(%p)->(%x,%u): stub\n", iface, flags, milliseconds);

    return E_NOTIMPL;
}

/*** IDirectDrawStreamSample methods ***/
static HRESULT WINAPI ddraw_sample_GetSurface(IDirectDrawStreamSample *iface, IDirectDrawSurface **ddraw_surface,
                                                             RECT *rect)
{
    struct ddraw_sample *sample = impl_from_IDirectDrawStreamSample(iface);

    TRACE("(%p)->(%p,%p)\n", iface, ddraw_surface, rect);

    if (ddraw_surface)
    {
        *ddraw_surface = sample->surface;
        if (*ddraw_surface)
            IDirectDrawSurface_AddRef(*ddraw_surface);
    }

    if (rect)
        *rect = sample->rect;

    return S_OK;
}

static HRESULT WINAPI ddraw_sample_SetRect(IDirectDrawStreamSample *iface, const RECT *rect)
{
    FIXME("(%p)->(%p): stub\n", iface, rect);

    return E_NOTIMPL;
}

static const struct IDirectDrawStreamSampleVtbl DirectDrawStreamSample_Vtbl =
{
    /*** IUnknown methods ***/
    ddraw_sample_QueryInterface,
    ddraw_sample_AddRef,
    ddraw_sample_Release,
    /*** IStreamSample methods ***/
    ddraw_sample_GetMediaStream,
    ddraw_sample_GetSampleTimes,
    ddraw_sample_SetSampleTimes,
    ddraw_sample_Update,
    ddraw_sample_CompletionStatus,
    /*** IDirectDrawStreamSample methods ***/
    ddraw_sample_GetSurface,
    ddraw_sample_SetRect
};

static HRESULT ddrawstreamsample_create(struct ddraw_stream *parent, IDirectDrawSurface *surface,
    const RECT *rect, IDirectDrawStreamSample **ddraw_stream_sample)
{
    struct ddraw_sample *object;
    HRESULT hr;

    TRACE("(%p)\n", ddraw_stream_sample);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if (!object)
        return E_OUTOFMEMORY;

    object->IDirectDrawStreamSample_iface.lpVtbl = &DirectDrawStreamSample_Vtbl;
    object->ref = 1;
    object->parent = parent;
    object->update_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    IAMMediaStream_AddRef(&parent->IAMMediaStream_iface);
    ++parent->sample_refs;

    if (surface)
    {
        object->surface = surface;
        IDirectDrawSurface_AddRef(surface);
    }
    else
    {
        DDSURFACEDESC desc;
        IDirectDraw *ddraw;

        hr = IDirectDrawMediaStream_GetDirectDraw(&parent->IDirectDrawMediaStream_iface, &ddraw);
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

        if (parent->peer)
        {
            const VIDEOINFOHEADER *video_info = (const VIDEOINFOHEADER *)parent->mt.pbFormat;
            desc.dwWidth = video_info->bmiHeader.biWidth;
            desc.dwHeight = abs(video_info->bmiHeader.biHeight);
        }
        else
        {
            if (parent->format.dwFlags & DDSD_WIDTH)
                desc.dwWidth = parent->format.dwWidth;
            if (parent->format.dwFlags & DDSD_HEIGHT)
                desc.dwWidth = parent->format.dwHeight;
        }

        if (parent->format.dwFlags & DDSD_PIXELFORMAT)
            desc.ddpfPixelFormat = parent->format.ddpfPixelFormat;

        hr = IDirectDraw_CreateSurface(ddraw, &desc, &object->surface, NULL);
        IDirectDraw_Release(ddraw);
        if (FAILED(hr))
        {
            ERR("failed to create surface, 0x%08x\n", hr);
            IDirectDrawStreamSample_Release(&object->IDirectDrawStreamSample_iface);
            return hr;
        }
    }

    if (rect)
        object->rect = *rect;
    else if (object->surface)
    {
        DDSURFACEDESC desc = { sizeof(desc) };
        hr = IDirectDrawSurface_GetSurfaceDesc(object->surface, &desc);
        if (hr == S_OK)
            SetRect(&object->rect, 0, 0, desc.dwWidth, desc.dwHeight);
    }

    *ddraw_stream_sample = &object->IDirectDrawStreamSample_iface;

    return S_OK;
}
