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
#include "qedit.h"

#include "wine/strmbase.h"
#include "wine/list.h"

#include "amstream_private.h"

#include "ddstream.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(amstream);

static HRESULT ddrawstreamsample_create(IDirectDrawMediaStream *parent, IDirectDrawSurface *surface,
    const RECT *rect, IDirectDrawStreamSample **ddraw_stream_sample);
static HRESULT audiostreamsample_create(IAudioMediaStream *parent, IAudioData *audio_data, IAudioStreamSample **audio_stream_sample);

typedef struct {
    BaseInputPin pin;
    IAMMediaStream IAMMediaStream_iface;
    IDirectDrawMediaStream IDirectDrawMediaStream_iface;
    IMultiMediaStream* parent;
    MSPID purpose_id;
    STREAM_TYPE stream_type;
    IDirectDraw7 *ddraw;
    CRITICAL_SECTION critical_section;
} DirectDrawMediaStreamImpl;

static inline DirectDrawMediaStreamImpl *impl_from_DirectDrawMediaStream_IPin(IPin *iface)
{
    return CONTAINING_RECORD(iface, DirectDrawMediaStreamImpl, pin.pin.IPin_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI DirectDrawMediaStreamImpl_IPin_QueryInterface(IPin *iface, REFIID riid, void **ret_iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IPin(iface);

    TRACE("(%p/%p)->(%s,%p)\n", iface, This, debugstr_guid(riid), ret_iface);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IPin))
    {
        IPin_AddRef(iface);
        *ret_iface = iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IMemInputPin))
    {
        IPin_AddRef(iface);
        *ret_iface = &This->pin.IMemInputPin_iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IMediaStream) ||
        IsEqualGUID(riid, &IID_IAMMediaStream))
    {
        IPin_AddRef(iface);
        *ret_iface = &This->IAMMediaStream_iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IDirectDrawMediaStream))
    {
        IPin_AddRef(iface);
        *ret_iface = &This->IDirectDrawMediaStream_iface;
        return S_OK;
    }

    ERR("(%p)->(%s,%p),not found\n", This, debugstr_guid(riid), ret_iface);
    return E_NOINTERFACE;
}

static ULONG WINAPI DirectDrawMediaStreamImpl_IPin_Release(IPin *iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IPin(iface);
    ULONG ref = InterlockedDecrement(&This->pin.pin.refCount);

    TRACE("(%p/%p)->(): new ref = %u\n", iface, This, ref);

    if (!ref)
    {
        if (This->ddraw)
            IDirectDraw7_Release(This->ddraw);
        DeleteCriticalSection(&This->critical_section);
        BaseInputPin_Destroy(&This->pin);
    }

    return ref;
}

static const IPinVtbl DirectDrawMediaStreamImpl_IPin_Vtbl =
{
    DirectDrawMediaStreamImpl_IPin_QueryInterface,
    BasePinImpl_AddRef,
    DirectDrawMediaStreamImpl_IPin_Release,
    BaseInputPinImpl_Connect,
    BaseInputPinImpl_ReceiveConnection,
    BasePinImpl_Disconnect,
    BasePinImpl_ConnectedTo,
    BasePinImpl_ConnectionMediaType,
    BasePinImpl_QueryPinInfo,
    BasePinImpl_QueryDirection,
    BasePinImpl_QueryId,
    BasePinImpl_QueryAccept,
    BasePinImpl_EnumMediaTypes,
    BasePinImpl_QueryInternalConnections,
    BaseInputPinImpl_EndOfStream,
    BaseInputPinImpl_BeginFlush,
    BaseInputPinImpl_EndFlush,
    BasePinImpl_NewSegment,
};

static inline DirectDrawMediaStreamImpl *impl_from_DirectDrawMediaStream_IAMMediaStream(IAMMediaStream *iface)
{
    return CONTAINING_RECORD(iface, DirectDrawMediaStreamImpl, IAMMediaStream_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_QueryInterface(IAMMediaStream *iface,
                                                        REFIID riid, void **ret_iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    return IPin_QueryInterface(&This->pin.pin.IPin_iface, riid, ret_iface);
}

static ULONG WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_AddRef(IAMMediaStream *iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    return IPin_AddRef(&This->pin.pin.IPin_iface);
}

static ULONG WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_Release(IAMMediaStream *iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    return IPin_Release(&This->pin.pin.IPin_iface);
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

    FIXME("(%p/%p)->(%u) stub!\n", This, iface, state);

    return S_FALSE;
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

    This->pin.pin.pinInfo.pFilter = (IBaseFilter *)media_stream_filter;

    return S_OK;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IAMMediaStream_JoinFilterGraph(IAMMediaStream *iface, IFilterGraph *filtergraph)
{
    DirectDrawMediaStreamImpl *This = impl_from_DirectDrawMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%p) stub!\n", This, iface, filtergraph);

    return S_FALSE;
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

static inline DirectDrawMediaStreamImpl *impl_from_IDirectDrawMediaStream(IDirectDrawMediaStream *iface)
{
    return CONTAINING_RECORD(iface, DirectDrawMediaStreamImpl, IDirectDrawMediaStream_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_QueryInterface(IDirectDrawMediaStream *iface,
        REFIID riid, void **ret_iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);

    return IPin_QueryInterface(&This->pin.pin.IPin_iface, riid, ret_iface);
}

static ULONG WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_AddRef(IDirectDrawMediaStream *iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);

    return IPin_AddRef(&This->pin.pin.IPin_iface);
}

static ULONG WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_Release(IDirectDrawMediaStream *iface)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);

    return IPin_Release(&This->pin.pin.IPin_iface);
}

/*** IMediaStream methods ***/
static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetMultiMediaStream(IDirectDrawMediaStream *iface,
        IMultiMediaStream **multi_media_stream)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);
    TRACE("(%p/%p)->(%p)\n", This, iface, multi_media_stream);
    return IAMMediaStream_GetMultiMediaStream(&This->IAMMediaStream_iface, multi_media_stream);
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetInformation(IDirectDrawMediaStream *iface,
        MSPID *purpose_id, STREAM_TYPE *type)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);
    TRACE("(%p/%p)->(%p,%p)\n", This, iface, purpose_id, type);
    return IAMMediaStream_GetInformation(&This->IAMMediaStream_iface, purpose_id, type);
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_SetSameFormat(IDirectDrawMediaStream *iface,
        IMediaStream *pStreamThatHasDesiredFormat, DWORD dwFlags)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);
    TRACE("(%p/%p)->(%p,%x)\n", This, iface, pStreamThatHasDesiredFormat, dwFlags);
    return IAMMediaStream_SetSameFormat(&This->IAMMediaStream_iface, pStreamThatHasDesiredFormat, dwFlags);
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_AllocateSample(IDirectDrawMediaStream *iface,
        DWORD dwFlags, IStreamSample **ppSample)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);
    TRACE("(%p/%p)->(%x,%p)\n", This, iface, dwFlags, ppSample);
    return IAMMediaStream_AllocateSample(&This->IAMMediaStream_iface, dwFlags, ppSample);
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_CreateSharedSample(IDirectDrawMediaStream *iface,
        IStreamSample *pExistingSample, DWORD dwFlags, IStreamSample **ppSample)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);
    TRACE("(%p/%p)->(%p,%x,%p)\n", This, iface, pExistingSample, dwFlags, ppSample);
    return IAMMediaStream_CreateSharedSample(&This->IAMMediaStream_iface, pExistingSample, dwFlags, ppSample);
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_SendEndOfStream(IDirectDrawMediaStream *iface,
        DWORD dwFlags)
{
    DirectDrawMediaStreamImpl *This = impl_from_IDirectDrawMediaStream(iface);
    TRACE("(%p/%p)->(%x)\n", This, iface, dwFlags);
    return IAMMediaStream_SendEndOfStream(&This->IAMMediaStream_iface, dwFlags);
}

/*** IDirectDrawMediaStream methods ***/
static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_GetFormat(IDirectDrawMediaStream *iface,
        DDSURFACEDESC *current_format, IDirectDrawPalette **palette,
        DDSURFACEDESC *desired_format, DWORD *flags)
{
    FIXME("(%p)->(%p,%p,%p,%p) stub!\n", iface, current_format, palette, desired_format,
            flags);

    return MS_E_NOSTREAM;

}

static HRESULT WINAPI DirectDrawMediaStreamImpl_IDirectDrawMediaStream_SetFormat(IDirectDrawMediaStream *iface,
        const DDSURFACEDESC *pDDSurfaceDesc, IDirectDrawPalette *pDirectDrawPalette)
{
    FIXME("(%p)->(%p,%p) stub!\n", iface, pDDSurfaceDesc, pDirectDrawPalette);

    return E_NOTIMPL;
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

HRESULT WINAPI DirectDrawMediaStreamImpl_BasePinImpl_CheckMediaType(BasePin *This, const AM_MEDIA_TYPE *pmt)
{
    TRACE("Checking media type %s - %s\n", debugstr_guid(&pmt->majortype), debugstr_guid(&pmt->subtype));

    if (IsEqualGUID(&pmt->majortype, &MEDIATYPE_Video))
    {
        if (IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_RGB1) ||
            IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_RGB4) ||
            IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_RGB8)  ||
            IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_RGB565) ||
            IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_RGB555) ||
            IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_RGB24) ||
            IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_RGB32))
        {
            TRACE("Video sub-type %s matches\n", debugstr_guid(&pmt->subtype));
            return S_OK;
        }
    }

    return S_FALSE;
}

static HRESULT WINAPI DirectDrawMediaStreamImpl_BasePinImpl_GetMediaType(BasePin *This, int index, AM_MEDIA_TYPE *amt)
{
    /* FIXME: Reset structure as we only fill majortype and minortype for now */
    ZeroMemory(amt, sizeof(*amt));

    amt->majortype = MEDIATYPE_Video;

    switch (index)
    {
        case 0:
            amt->subtype = MEDIASUBTYPE_RGB1;
            break;
        case 1:
            amt->subtype = MEDIASUBTYPE_RGB4;
            break;
        case 2:
            amt->subtype = MEDIASUBTYPE_RGB8;
            break;
        case 3:
            amt->subtype = MEDIASUBTYPE_RGB565;
            break;
        case 4:
            amt->subtype = MEDIASUBTYPE_RGB555;
            break;
        case 5:
            amt->subtype = MEDIASUBTYPE_RGB24;
            break;
        case 6:
            amt->subtype = MEDIASUBTYPE_RGB32;
            break;
        default:
            return S_FALSE;
    }

    return S_OK;
}

HRESULT WINAPI DirectDrawMediaStreamImpl_BaseInputPinImpl_Receive(BaseInputPin *This, IMediaSample *pSample)
{
    DirectDrawMediaStreamImpl *stream = impl_from_DirectDrawMediaStream_IPin(&This->pin.IPin_iface);

    FIXME("(%p)->(%p) stub!\n", stream, pSample);

    return E_NOTIMPL;
}

static const BaseInputPinFuncTable DirectDrawMediaStreamImpl_BaseInputPinFuncTable = {
    {
        DirectDrawMediaStreamImpl_BasePinImpl_CheckMediaType,
        NULL,
        BasePinImpl_GetMediaTypeVersion,
        DirectDrawMediaStreamImpl_BasePinImpl_GetMediaType,
    },
    DirectDrawMediaStreamImpl_BaseInputPinImpl_Receive,
};

HRESULT ddrawmediastream_create(IMultiMediaStream *parent, const MSPID *purpose_id,
        STREAM_TYPE stream_type, IAMMediaStream **media_stream)
{
    DirectDrawMediaStreamImpl *object;

    TRACE("(%p,%s,%p)\n", parent, debugstr_guid(purpose_id), media_stream);

    {
        IPin *pin = NULL;
        {
            PIN_INFO info;
            info.pFilter = NULL;
            info.dir = PINDIR_INPUT;
            /* Pin name is "I{guid MSPID_PrimaryVideo or MSPID_PrimaryAudio}" */
            info.achName[0] = L'I';
            StringFromGUID2(purpose_id, &info.achName[1], MAX_PIN_NAME - 1);
            {
                HRESULT hr = BaseInputPin_Construct(
                    &DirectDrawMediaStreamImpl_IPin_Vtbl,
                    sizeof(DirectDrawMediaStreamImpl),
                    &info,
                    &DirectDrawMediaStreamImpl_BaseInputPinFuncTable,
                    NULL,
                    NULL,
                    &pin);
                if (FAILED(hr))
                {
                    return hr;
                }
            }
        }
        object = impl_from_DirectDrawMediaStream_IPin(pin);
    }

    object->IAMMediaStream_iface.lpVtbl = &DirectDrawMediaStreamImpl_IAMMediaStream_Vtbl;
    object->IDirectDrawMediaStream_iface.lpVtbl = &DirectDrawMediaStreamImpl_IDirectDrawMediaStream_Vtbl;

    object->parent = parent;
    object->purpose_id = *purpose_id;
    object->stream_type = stream_type;
    object->ddraw = NULL;

    InitializeCriticalSection(&object->critical_section);
    object->pin.pin.pCritSec = &object->critical_section;

    *media_stream = &object->IAMMediaStream_iface;

    return S_OK;
}

typedef struct {
    struct list entry;
    IAudioData *audio_data;
    BOOL update_pending;
    BOOL updating;
    HRESULT update_result;
    HANDLE update_complete_event;
    HANDLE update_complete_internal_event;
} QueuedAudioStreamSample;

typedef struct {
    BaseInputPin pin;
    IAMMediaStream IAMMediaStream_iface;
    IAudioMediaStream IAudioMediaStream_iface;
    LONG ref;
    IMultiMediaStream* parent;
    MSPID purpose_id;
    STREAM_TYPE stream_type;
    FILTER_STATE state;
    struct list sample_queue;
    HANDLE sample_queued_event;
    CRITICAL_SECTION critical_section;
} AudioMediaStreamImpl;

static inline AudioMediaStreamImpl *impl_from_AudioMediaStream_IPin(IPin *iface)
{
    return CONTAINING_RECORD(iface, AudioMediaStreamImpl, pin.pin.IPin_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI AudioMediaStreamImpl_IPin_QueryInterface(IPin *iface, REFIID riid, void **ret_iface)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IPin(iface);

    TRACE("(%p/%p)->(%s,%p)\n", iface, This, debugstr_guid(riid), ret_iface);

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IPin))
    {
        IPin_AddRef(iface);
        *ret_iface = iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IMemInputPin))
    {
        IPin_AddRef(iface);
        *ret_iface = &This->pin.IMemInputPin_iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IMediaStream) ||
        IsEqualGUID(riid, &IID_IAMMediaStream))
    {
        IPin_AddRef(iface);
        *ret_iface = &This->IAMMediaStream_iface;
        return S_OK;
    }
    else if (IsEqualGUID(riid, &IID_IAudioMediaStream))
    {
        IPin_AddRef(iface);
        *ret_iface = &This->IAudioMediaStream_iface;
        return S_OK;
    }

    ERR("(%p)->(%s,%p),not found\n", This, debugstr_guid(riid), ret_iface);
    return E_NOINTERFACE;
}

static ULONG WINAPI AudioMediaStreamImpl_IPin_Release(IPin *iface)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IPin(iface);
    ULONG ref = InterlockedDecrement(&This->pin.pin.refCount);

    TRACE("(%p/%p)->(): new ref = %d\n", iface, This, ref);

    if (!ref)
    {
        CloseHandle(This->sample_queued_event);
        // TODO: do something with queued samples.
        DeleteCriticalSection(&This->critical_section);
        BaseInputPin_Destroy(&This->pin);
    }
    return ref;
}

static HRESULT WINAPI AudioMediaStreamImpl_IPin_EndOfStream(IPin *iface)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IPin(iface);

    TRACE("(%p/%p)->()\n", iface, This);

    {
        HRESULT hr = BaseInputPinImpl_EndOfStream(&This->pin.pin.IPin_iface);
        if (FAILED(hr))
        {
            return hr;
        }
        EnterCriticalSection(&This->critical_section);
        while (!list_empty(&This->sample_queue))
        {
            QueuedAudioStreamSample *output_sample =
                LIST_ENTRY(list_head(&This->sample_queue), QueuedAudioStreamSample, entry);

            list_remove(&output_sample->entry);
            output_sample->update_result = output_sample->updating? S_OK : MS_S_ENDOFSTREAM;
            output_sample->update_pending = FALSE;
            output_sample->updating = FALSE;
            if (output_sample->update_complete_event)
            {
                SetEvent(output_sample->update_complete_event);
            }
            SetEvent(output_sample->update_complete_internal_event);
        }
        LeaveCriticalSection(&This->critical_section);
        return hr;
    }
}

static HRESULT WINAPI AudioMediaStreamImpl_IPin_BeginFlush(IPin *iface)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IPin(iface);

    TRACE("(%p/%p)->()\n", iface, This);

    {
        HRESULT hr = BaseInputPinImpl_BeginFlush(&This->pin.pin.IPin_iface);
        if (FAILED(hr))
        {
            return hr;
        }
        SetEvent(This->sample_queued_event);
        return hr;
    }
}

static const IPinVtbl AudioMediaStreamImpl_IPin_Vtbl =
{
    AudioMediaStreamImpl_IPin_QueryInterface,
    BasePinImpl_AddRef,
    AudioMediaStreamImpl_IPin_Release,
    BaseInputPinImpl_Connect,
    BaseInputPinImpl_ReceiveConnection,
    BasePinImpl_Disconnect,
    BasePinImpl_ConnectedTo,
    BasePinImpl_ConnectionMediaType,
    BasePinImpl_QueryPinInfo,
    BasePinImpl_QueryDirection,
    BasePinImpl_QueryId,
    BasePinImpl_QueryAccept,
    BasePinImpl_EnumMediaTypes,
    BasePinImpl_QueryInternalConnections,
    AudioMediaStreamImpl_IPin_EndOfStream,
    AudioMediaStreamImpl_IPin_BeginFlush,
    BaseInputPinImpl_EndFlush,
    BasePinImpl_NewSegment,
};

static inline AudioMediaStreamImpl *impl_from_AudioMediaStream_IAMMediaStream(IAMMediaStream *iface)
{
    return CONTAINING_RECORD(iface, AudioMediaStreamImpl, IAMMediaStream_iface);
}

/*** IUnknown methods ***/
static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_QueryInterface(IAMMediaStream *iface,
                                                        REFIID riid, void **ret_iface)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    return IPin_QueryInterface(&This->pin.pin.IPin_iface, riid, ret_iface);
}

static ULONG WINAPI AudioMediaStreamImpl_IAMMediaStream_AddRef(IAMMediaStream *iface)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    return IPin_AddRef(&This->pin.pin.IPin_iface);
}

static ULONG WINAPI AudioMediaStreamImpl_IAMMediaStream_Release(IAMMediaStream *iface)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    return IPin_Release(&This->pin.pin.IPin_iface);
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

    TRACE("(%p/%p)->(%u)\n", This, iface, state);

    EnterCriticalSection(&This->critical_section);
    switch (state)
    {
    case State_Stopped:
        if (State_Stopped != This->state)
        {
            This->state = state;
            SetEvent(This->sample_queued_event);
        }
        break;
    case State_Paused:
    case State_Running:
        This->state = state;
        break;
    default:
        return E_INVALIDARG;
    }
    LeaveCriticalSection(&This->critical_section);

    return S_OK;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_JoinAMMultiMediaStream(IAMMediaStream *iface, IAMMultiMediaStream *am_multi_media_stream)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", This, iface, am_multi_media_stream);

    This->parent = (IMultiMediaStream *)am_multi_media_stream;

    return S_OK;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_JoinFilter(IAMMediaStream *iface, IMediaStreamFilter *media_stream_filter)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", This, iface, media_stream_filter);

    This->pin.pin.pinInfo.pFilter = (IBaseFilter *)media_stream_filter;

    return S_OK;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAMMediaStream_JoinFilterGraph(IAMMediaStream *iface, IFilterGraph *filtergraph)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IAMMediaStream(iface);

    FIXME("(%p/%p)->(%p) stub!\n", This, iface, filtergraph);

    return S_FALSE;
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
    return IPin_QueryInterface(&This->pin.pin.IPin_iface, riid, ret_iface);
}

static ULONG WINAPI AudioMediaStreamImpl_IAudioMediaStream_AddRef(IAudioMediaStream *iface)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    TRACE("(%p/%p)\n", iface, This);
    return IPin_AddRef(&This->pin.pin.IPin_iface);
}

static ULONG WINAPI AudioMediaStreamImpl_IAudioMediaStream_Release(IAudioMediaStream *iface)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    TRACE("(%p/%p)\n", iface, This);
    return IPin_Release(&This->pin.pin.IPin_iface);
}

/*** IMediaStream methods ***/
static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_GetMultiMediaStream(IAudioMediaStream *iface,
        IMultiMediaStream **multi_media_stream)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    TRACE("(%p/%p)->(%p)\n", iface, This, multi_media_stream);
    return IAMMediaStream_GetMultiMediaStream(&This->IAMMediaStream_iface, multi_media_stream);
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_GetInformation(IAudioMediaStream *iface,
        MSPID *purpose_id, STREAM_TYPE *type)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    TRACE("(%p/%p)->(%p,%p)\n", iface, This, purpose_id, type);
    return IAMMediaStream_GetInformation(&This->IAMMediaStream_iface, purpose_id, type);
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_SetSameFormat(IAudioMediaStream *iface,
        IMediaStream *stream_format, DWORD flags)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    TRACE("(%p/%p)->(%p,%x)\n", iface, This, stream_format, flags);
    return IAMMediaStream_SetSameFormat(&This->IAMMediaStream_iface, stream_format, flags);
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_AllocateSample(IAudioMediaStream *iface,
        DWORD flags, IStreamSample **sample)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    TRACE("(%p/%p)->(%x,%p)\n", iface, This, flags, sample);
    return IAMMediaStream_AllocateSample(&This->IAMMediaStream_iface, flags, sample);
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_CreateSharedSample(IAudioMediaStream *iface,
        IStreamSample *existing_sample, DWORD flags, IStreamSample **sample)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    TRACE("(%p/%p)->(%p,%x,%p)\n", iface, This, existing_sample, flags, sample);
    return IAMMediaStream_CreateSharedSample(&This->IAMMediaStream_iface, existing_sample, flags, sample);
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_SendEndOfStream(IAudioMediaStream *iface,
        DWORD flags)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    TRACE("(%p/%p)->(%x)\n", iface, This, flags);
    return IAMMediaStream_SendEndOfStream(&This->IAMMediaStream_iface, flags);
}

/*** IAudioMediaStream methods ***/
static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_GetFormat(IAudioMediaStream *iface, WAVEFORMATEX *wave_format_current)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);

    TRACE("(%p/%p)->(%p)\n", iface, This, wave_format_current);

    if (!wave_format_current)
        return E_POINTER;

    if (!This->pin.pin.mtCurrent.pbFormat)
    {
        return E_NOTDETERMINED;
    }

    *wave_format_current = *(const WAVEFORMATEX *)This->pin.pin.mtCurrent.pbFormat;

    return S_OK;
}

static HRESULT WINAPI AudioMediaStreamImpl_IAudioMediaStream_SetFormat(IAudioMediaStream *iface, const WAVEFORMATEX *wave_format)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);

    FIXME("(%p/%p)->(%p) stub!\n", iface, This, wave_format);

    return E_NOTIMPL;
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

HRESULT WINAPI AudioMediaStreamImpl_BasePinImpl_CheckMediaType(BasePin *This, const AM_MEDIA_TYPE *pmt)
{
    TRACE("Checking media type %s - %s\n", debugstr_guid(&pmt->majortype), debugstr_guid(&pmt->subtype));

    if (IsEqualGUID(&pmt->majortype, &MEDIATYPE_Audio))
    {
        if (IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_PCM))
        {
            TRACE("Audio sub-type %s matches\n", debugstr_guid(&pmt->subtype));
            return S_OK;
        }
    }

    return S_FALSE;
}

static HRESULT WINAPI AudioMediaStreamImpl_BasePinImpl_GetMediaType(BasePin *This, int index, AM_MEDIA_TYPE *amt)
{
    /* FIXME: Reset structure as we only fill majortype and minortype for now */
    ZeroMemory(amt, sizeof(*amt));

    if (index)
    {
        return S_FALSE;
    }

    amt->majortype = MEDIATYPE_Audio;
    amt->subtype = MEDIASUBTYPE_PCM;

    return S_OK;
}

HRESULT WINAPI AudioMediaStreamImpl_BaseInputPinImpl_Receive(BaseInputPin *pin, IMediaSample *pSample)
{
    AudioMediaStreamImpl *This = impl_from_AudioMediaStream_IPin(&pin->pin.IPin_iface);
    HRESULT hr = S_OK;

    DWORD sample_length = 0;
    BYTE *sample_pointer = NULL;
    DWORD sample_position = 0;

    TRACE("(%p)->(%p)\n", This, pSample);

    sample_length = IMediaSample_GetActualDataLength(pSample);
    hr = IMediaSample_GetPointer(pSample, &sample_pointer);
    if (FAILED(hr))
    {
        return hr;
    }

    for (;;)
    {
        EnterCriticalSection(&This->critical_section);
        if (State_Stopped == This->state)
        {
            hr = VFW_E_WRONG_STATE;
            goto out_critical_section;
        }
        if (This->pin.flushing)
        {
            hr = S_FALSE;
            goto out_critical_section;
        }
        if (!list_empty(&This->sample_queue))
        {
            DWORD output_length = 0;
            BYTE *output_pointer = NULL;
            DWORD output_position = 0;

            DWORD advance = 0;

            QueuedAudioStreamSample *output_sample =
                    LIST_ENTRY(list_head(&This->sample_queue), QueuedAudioStreamSample, entry);

            output_sample->update_pending = FALSE;
            output_sample->updating = TRUE;

            hr = IAudioData_GetInfo(output_sample->audio_data, &output_length,
                    &output_pointer, &output_position);
            if (FAILED(hr))
            {
                goto out_critical_section;
            }

            advance = min(sample_length - sample_position, output_length - output_position);
            CopyMemory(&output_pointer[output_position], &sample_pointer[sample_position], advance);
            output_position += advance;
            sample_position += advance;

            hr = IAudioData_SetActual(output_sample->audio_data, output_position);
            if (FAILED(hr))
            {
                goto out_critical_section;
            }

            if (output_length == output_position)
            {
                list_remove(&output_sample->entry);
                output_sample->updating = FALSE;
                output_sample->update_result = S_OK;
                if (output_sample->update_complete_event)
                {
                    SetEvent(output_sample->update_complete_event);
                }
                SetEvent(output_sample->update_complete_internal_event);
            }
            if (sample_length == sample_position)
            {
                hr = S_OK;
                goto out_critical_section;
            }
        }
        LeaveCriticalSection(&This->critical_section);
        WaitForSingleObject(This->sample_queued_event, INFINITE);
    }

out_critical_section:
    LeaveCriticalSection(&This->critical_section);

    return hr;
}

static const BaseInputPinFuncTable AudioMediaStreamImpl_BaseInputPinFuncTable = {
    {
        AudioMediaStreamImpl_BasePinImpl_CheckMediaType,
        NULL,
        BasePinImpl_GetMediaTypeVersion,
        AudioMediaStreamImpl_BasePinImpl_GetMediaType,
    },
    AudioMediaStreamImpl_BaseInputPinImpl_Receive,
};

HRESULT audiomediastream_create(IMultiMediaStream *parent, const MSPID *purpose_id,
        STREAM_TYPE stream_type, IAMMediaStream **media_stream)
{
    AudioMediaStreamImpl *object = NULL;

    TRACE("(%p,%s,%p)\n", parent, debugstr_guid(purpose_id), media_stream);

    {
        IPin *pin = NULL;
        {
            PIN_INFO info;
            info.pFilter = NULL;
            info.dir = PINDIR_INPUT;
            /* Pin name is "I{guid MSPID_PrimaryVideo or MSPID_PrimaryAudio}" */
            info.achName[0] = L'I';
            StringFromGUID2(purpose_id, &info.achName[1], MAX_PIN_NAME - 1);
            {
                HRESULT hr = BaseInputPin_Construct(
                    &AudioMediaStreamImpl_IPin_Vtbl,
                    sizeof(AudioMediaStreamImpl),
                    &info,
                    &AudioMediaStreamImpl_BaseInputPinFuncTable,
                    NULL,
                    NULL,
                    &pin);
                if (FAILED(hr))
                {
                    return hr;
                }
            }
        }
        object = impl_from_AudioMediaStream_IPin(pin);
    }

    object->IAMMediaStream_iface.lpVtbl = &AudioMediaStreamImpl_IAMMediaStream_Vtbl;
    object->IAudioMediaStream_iface.lpVtbl = &AudioMediaStreamImpl_IAudioMediaStream_Vtbl;

    object->parent = parent;
    object->purpose_id = *purpose_id;
    object->stream_type = stream_type;
    object->state = State_Stopped;
    object->sample_queued_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    list_init(&object->sample_queue);

    InitializeCriticalSection(&object->critical_section);
    object->pin.pin.pCritSec = &object->critical_section;

    *media_stream = &object->IAMMediaStream_iface;

    return S_OK;
}

static HRESULT audiomediastream_update_sample(IAudioMediaStream *iface, QueuedAudioStreamSample *sample,
        DWORD flags, HANDLE event, PAPCFUNC func_APC, DWORD_PTR APC_data, HANDLE default_event, IAudioData *audio_data)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    HRESULT hr;

    TRACE("(%p/%p)->(%p, %u, %p, %p, %08lx)\n", iface, This, sample, flags, event, func_APC, APC_data);

    if (event && func_APC)
    {
        return E_INVALIDARG;
    }

    if ((event || func_APC) && (flags & SSUPDATE_ASYNC))
    {
        return E_INVALIDARG;
    }

    if (func_APC)
    {
        FIXME("APC support is not implemented!");
        return E_NOTIMPL;
    }

    EnterCriticalSection(&This->critical_section);
    if (sample->update_pending || sample->updating)
    {
        hr = MS_E_BUSY;
    }
    else if (This->pin.end_of_stream)
    {
        hr = MS_S_ENDOFSTREAM;
    }
    else
    {
        hr = IAudioData_SetActual(audio_data, 0);
        if (SUCCEEDED(hr))
        {
            sample->audio_data = audio_data;
            sample->update_result = E_ABORT;
            sample->update_pending = TRUE;
            sample->updating = FALSE;
            sample->update_complete_event = event;
            sample->update_complete_internal_event = default_event;
            list_add_tail(&This->sample_queue, &sample->entry);

            if (sample->update_complete_event)
            {
                ResetEvent(sample->update_complete_event);
            }
            ResetEvent(sample->update_complete_internal_event);

            SetEvent(This->sample_queued_event);

            hr = MS_S_PENDING;
        }
    }
    LeaveCriticalSection(&This->critical_section);

    if (MS_S_PENDING == hr)
    {
        if (!event && !func_APC && !(flags & SSUPDATE_ASYNC))
        {
            WaitForSingleObject(sample->update_complete_internal_event, INFINITE);
            hr = sample->update_result;
        }
    }

    return hr;
}

static HRESULT audiomediastream_sample_update_completion_status(IAudioMediaStream *iface,
        QueuedAudioStreamSample *sample, DWORD flags, DWORD milliseconds)
{
    AudioMediaStreamImpl *This = impl_from_IAudioMediaStream(iface);
    HRESULT hr;

    TRACE("(%p/%p)->(%p, %u, %u)\n", iface, This, sample, flags, milliseconds);

    EnterCriticalSection(&This->critical_section);
    if (sample->update_pending)
    {
        if ((flags & COMPSTAT_NOUPDATEOK) || (flags & COMPSTAT_ABORT))
        {
            list_remove(&sample->entry);
            sample->update_result = MS_S_NOUPDATE;
            sample->update_pending = FALSE;
            sample->updating = FALSE;
            if (sample->update_complete_event)
            {
                SetEvent(sample->update_complete_event);
            }
            /* TODO: queue APC */
            hr = sample->update_result;
        }
        else
        {
            if (flags & COMPSTAT_WAIT)
            {
                LeaveCriticalSection(&This->critical_section);
                WaitForSingleObject(sample->update_complete_internal_event, milliseconds);
                EnterCriticalSection(&This->critical_section);
                hr = sample->update_result;
            }
            else
            {
                hr = MS_S_PENDING;
            }
        }
    }
    else if (sample->updating)
    {
        if (flags & COMPSTAT_ABORT)
        {
            list_remove(&sample->entry);
            sample->update_result = E_ABORT;
            sample->update_pending = FALSE;
            sample->updating = FALSE;
            if (sample->update_complete_event)
            {
                SetEvent(sample->update_complete_event);
            }
            /* TODO: queue APC */
            hr = sample->update_result;
        }
        else
        {
            if (flags & COMPSTAT_WAIT)
            {
                LeaveCriticalSection(&This->critical_section);
                WaitForSingleObject(sample->update_complete_internal_event, milliseconds);
                EnterCriticalSection(&This->critical_section);
                hr = sample->update_result;
            }
            else
            {
                hr = MS_S_PENDING;
            }
        }
    }
    else
    {
        hr = sample->update_result;
    }
    LeaveCriticalSection(&This->critical_section);

    return hr;
}

typedef struct {
    IDirectDrawStreamSample IDirectDrawStreamSample_iface;
    LONG ref;
    IMediaStream *parent;
    STREAM_TIME start_time;
    STREAM_TIME end_time;
    IDirectDrawSurface *surface;
    RECT rect;
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
        if (This->surface)
            IDirectDrawSurface_Release(This->surface);
        IMediaStream_Release(This->parent);
        HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

/*** IStreamSample methods ***/
static HRESULT WINAPI IDirectDrawStreamSampleImpl_GetMediaStream(IDirectDrawStreamSample *iface, IMediaStream **media_stream)
{
    IDirectDrawStreamSampleImpl *This = impl_from_IDirectDrawStreamSample(iface);

    TRACE("(%p)->(%p)\n", iface, media_stream);

    if (!media_stream)
    {
        return E_POINTER;
    }

    IMediaStream_AddRef(This->parent);
    *media_stream = This->parent;

    return S_OK;
}

static HRESULT WINAPI IDirectDrawStreamSampleImpl_GetSampleTimes(IDirectDrawStreamSample *iface, STREAM_TIME *start_time,
                                                                 STREAM_TIME *end_time, STREAM_TIME *current_time)
{
    IDirectDrawStreamSampleImpl *This = impl_from_IDirectDrawStreamSample(iface);

    TRACE("(%p)->(%p,%p,%p)\n", iface, start_time, end_time, current_time);

    if (!start_time || !end_time || !current_time)
    {
        return E_POINTER;
    }

    {
        IMultiMediaStream *multi_media_stream = NULL;
        {
            HRESULT hr = IMediaStream_GetMultiMediaStream(This->parent, &multi_media_stream);
            if (FAILED(hr))
            {
                return hr;
            }
        }
        {
            HRESULT hr = IMultiMediaStream_GetTime(multi_media_stream, current_time);
            IMultiMediaStream_Release(multi_media_stream);
            if (FAILED(hr))
            {
                return hr;
            }
        }
    }

    *start_time = This->start_time;
    *end_time = This->end_time;

    return S_OK;
}

static HRESULT WINAPI IDirectDrawStreamSampleImpl_SetSampleTimes(IDirectDrawStreamSample *iface, const STREAM_TIME *start_time,
                                                                 const STREAM_TIME *end_time)
{
    IDirectDrawStreamSampleImpl *This = impl_from_IDirectDrawStreamSample(iface);

    TRACE("(%p)->(%p,%p)\n", iface, start_time, end_time);

    if (!start_time || !end_time)
    {
        return E_POINTER;
    }

    This->start_time = *start_time;
    This->end_time = *end_time;

    return S_OK;
}

static HRESULT WINAPI IDirectDrawStreamSampleImpl_Update(IDirectDrawStreamSample *iface, DWORD flags, HANDLE event,
                                                         PAPCFUNC func_APC, DWORD APC_data)
{
    FIXME("(%p)->(%x,%p,%p,%u): stub\n", iface, flags, event, func_APC, APC_data);

    return E_NOTIMPL;
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
    object->parent = (IMediaStream*)parent;
    IMediaStream_AddRef(object->parent);

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

typedef struct {
    IAudioStreamSample IAudioStreamSample_iface;
    LONG ref;
    IMediaStream *parent;
    IAudioData *audio_data;
    STREAM_TIME start_time;
    STREAM_TIME end_time;
    HANDLE default_update_complete_event;
    QueuedAudioStreamSample queue_entry;
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
        HeapFree(GetProcessHeap(), 0, This);

    return ref;
}

/*** IStreamSample methods ***/
static HRESULT WINAPI IAudioStreamSampleImpl_GetMediaStream(IAudioStreamSample *iface, IMediaStream **media_stream)
{
    IAudioStreamSampleImpl *This = impl_from_IAudioStreamSample(iface);

    TRACE("(%p)->(%p)\n", iface, media_stream);

    if (!media_stream)
    {
        return E_POINTER;
    }

    IMediaStream_AddRef(This->parent);
    *media_stream = This->parent;

    return S_OK;
}

static HRESULT WINAPI IAudioStreamSampleImpl_GetSampleTimes(IAudioStreamSample *iface, STREAM_TIME *start_time,
                                                                 STREAM_TIME *end_time, STREAM_TIME *current_time)
{
    IAudioStreamSampleImpl *This = impl_from_IAudioStreamSample(iface);

    TRACE("(%p)->(%p,%p,%p)\n", iface, start_time, end_time, current_time);

    if (!start_time || !end_time || !current_time)
    {
        return E_POINTER;
    }

    {
        IMultiMediaStream *multi_media_stream = NULL;
        {
            HRESULT hr = IMediaStream_GetMultiMediaStream(This->parent, &multi_media_stream);
            if (FAILED(hr))
            {
                return hr;
            }
        }
        {
            HRESULT hr = IMultiMediaStream_GetTime(multi_media_stream, current_time);
            IMultiMediaStream_Release(multi_media_stream);
            if (FAILED(hr))
            {
                return hr;
            }
        }
    }

    *start_time = This->start_time;
    *end_time = This->end_time;

    return S_OK;
}

static HRESULT WINAPI IAudioStreamSampleImpl_SetSampleTimes(IAudioStreamSample *iface, const STREAM_TIME *start_time,
                                                                 const STREAM_TIME *end_time)
{
    IAudioStreamSampleImpl *This = impl_from_IAudioStreamSample(iface);

    TRACE("(%p)->(%p,%p)\n", iface, start_time, end_time);

    if (!start_time || !end_time)
    {
        return E_POINTER;
    }

    This->start_time = *start_time;
    This->end_time = *end_time;

    return S_OK;
}

static HRESULT WINAPI IAudioStreamSampleImpl_Update(IAudioStreamSample *iface, DWORD flags, HANDLE event,
                                                         PAPCFUNC func_APC, DWORD APC_data)
{
    IAudioStreamSampleImpl *This = impl_from_IAudioStreamSample(iface);

    TRACE("(%p)->(%x,%p,%p,%u)\n", iface, flags, event, func_APC, APC_data);

    return audiomediastream_update_sample((IAudioMediaStream *)This->parent, &This->queue_entry,
            flags, event, func_APC, APC_data, This->default_update_complete_event, This->audio_data);
}

static HRESULT WINAPI IAudioStreamSampleImpl_CompletionStatus(IAudioStreamSample *iface, DWORD flags, DWORD milliseconds)
{
    IAudioStreamSampleImpl *This = impl_from_IAudioStreamSample(iface);

    TRACE("(%p)->(%x,%u)\n", iface, flags, milliseconds);

    return audiomediastream_sample_update_completion_status((IAudioMediaStream *)This->parent, &This->queue_entry, flags, milliseconds);
}

/*** IAudioStreamSample methods ***/
static HRESULT WINAPI IAudioStreamSampleImpl_GetAudioData(IAudioStreamSample *iface, IAudioData **audio_data)
{
    IAudioStreamSampleImpl *This = impl_from_IAudioStreamSample(iface);

    TRACE("(%p)->(%p)\n", iface, audio_data);

    if (!audio_data)
    {
        return E_POINTER;
    }

    IAudioData_AddRef(This->audio_data);
    *audio_data = This->audio_data;

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

    TRACE("(%p)\n", audio_stream_sample);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(IAudioStreamSampleImpl));
    if (!object)
        return E_OUTOFMEMORY;

    object->IAudioStreamSample_iface.lpVtbl = &AudioStreamSample_Vtbl;
    object->ref = 1;
    object->parent = (IMediaStream*)parent;
    object->default_update_complete_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    object->audio_data = audio_data;

    // FIXME
    object->queue_entry.update_result = S_OK;
    object->queue_entry.update_pending = FALSE;
    object->queue_entry.updating = FALSE;

    *audio_stream_sample = (IAudioStreamSample*)&object->IAudioStreamSample_iface;

    return S_OK;
}
