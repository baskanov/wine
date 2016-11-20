/*
 * MPEG Audio Codec Filter
 *
 * Copyright 2016 Anton Baskanov
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

#include "config.h"

#include "quartz_private.h"
#include "pin.h"

#include "uuids.h"
#include "mmreg.h"
#include "windef.h"
#include "winbase.h"
#include "dshow.h"
#include "strmif.h"
#include "vfwmsgs.h"

#include <mpg123.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);

typedef struct MPEGAudioCodecImpl
{
    TransformFilter tf;

    mpg123_handle *mh;

    DWORD frame_size;
} MPEGAudioCodecImpl;

static inline MPEGAudioCodecImpl *impl_from_TransformFilter(TransformFilter *iface)
{
    return CONTAINING_RECORD(iface, MPEGAudioCodecImpl, tf.filter);
}

static HRESULT WINAPI MPEGAudioCodec_Receive(TransformFilter *const tf, IMediaSample *const input_sample)
{
    MPEGAudioCodecImpl *const This = impl_from_TransformFilter(tf);
    HRESULT hr = VFW_E_RUNTIME_ERROR;
    BYTE *input_pointer = NULL;
    const DWORD input_actual = IMediaSample_GetActualDataLength(input_sample);
    const BOOL preroll = (S_OK == IMediaSample_IsPreroll(input_sample));
    BOOL discontinuity = (S_OK == IMediaSample_IsDiscontinuity(input_sample));

    EnterCriticalSection(&This->tf.csReceive);

    if (discontinuity)
    {
        TRACE("Discontinuity\n");

        mpg123_close(This->mh);
        if (MPG123_OK != mpg123_open_feed(This->mh))
        {
            ERR("Unable to open mpg123 bitstream: %s\n", mpg123_strerror(This->mh));
            hr = VFW_E_RUNTIME_ERROR;
            goto out_critical_section;
        }
    }

    hr = IMediaSample_GetPointer(input_sample, &input_pointer);
    if (FAILED(hr))
    {
        ERR("Cannot get pointer to sample data (%x)\n", hr);
        goto out_critical_section;
    }

    TRACE("Feeding %u bytes\n", input_actual);

    if (MPG123_OK != mpg123_feed(This->mh, input_pointer, input_actual))
    {
        ERR("Unable to feed sample data: %s\n", mpg123_strerror(This->mh));
        hr = VFW_E_RUNTIME_ERROR;
        goto out_critical_section;
    }

    for (;;)
    {
        IMediaSample *output_sample = NULL;
        BYTE *output_pointer = NULL;
        DWORD output_size = 0;
        size_t output_actual = 0;

        hr = BaseOutputPinImpl_GetDeliveryBuffer((BaseOutputPin *)This->tf.ppPins[1], &output_sample, NULL, NULL, 0);
        if (FAILED(hr))
        {
            ERR("Unable to get delivery buffer (%x)\n", hr);
            goto out_critical_section;
        }

        output_size = IMediaSample_GetSize(output_sample);

        hr = IMediaSample_GetPointer(output_sample, &output_pointer);
        if (FAILED(hr))
        {
            ERR("Cannot get pointer to sample data (%x)\n", hr);
            goto out_output_sample;
        }

        for (;;)
        {
            const int error = mpg123_read(This->mh, output_pointer, output_size, &output_actual);
            if (MPG123_NEED_MORE == error)
            {
                if (0 == output_actual)
                {
                    TRACE("Need more\n");
                    hr = S_OK;
                    goto out_output_sample;
                }
            }
            else if (MPG123_NEW_FORMAT == error)
            {
                continue;
            }
            else if (MPG123_OK != error)
            {
                ERR("Unable to decode sample: %s\n", mpg123_strerror(This->mh));
                hr = VFW_E_RUNTIME_ERROR;
                goto out_output_sample;
            }
            break;
        }

        TRACE("Decoded %u bytes\n", (DWORD)output_actual);

        hr = IMediaSample_SetActualDataLength(output_sample, (DWORD)output_actual);
        if (FAILED(hr))
        {
            ERR("Cannot set sample actual data length (%x)\n", hr);
            goto out_output_sample;
        }

        IMediaSample_SetSyncPoint(output_sample, TRUE);
        IMediaSample_SetPreroll(output_sample, preroll);
        IMediaSample_SetDiscontinuity(output_sample, discontinuity);
        discontinuity = FALSE;

        /* TODO set time stamps */

        LeaveCriticalSection(&This->tf.csReceive);
        hr = BaseOutputPinImpl_Deliver((BaseOutputPin *)This->tf.ppPins[1], output_sample);
        EnterCriticalSection(&This->tf.csReceive);
        if (FAILED(hr))
        {
            ERR("Cannot deliver sample (%x)\n", hr);
            goto out_output_sample;
        }

        IMediaSample_Release(output_sample);
        continue;

out_output_sample:
        IMediaSample_Release(output_sample);
        break;
    }

out_critical_section:
    LeaveCriticalSection(&This->tf.csReceive);

    return hr;
}

static HRESULT WINAPI MPEGAudioCodec_SetMediaType(TransformFilter *const tf, const PIN_DIRECTION dir, const AM_MEDIA_TYPE *const pmt)
{
    MPEGAudioCodecImpl *const This = impl_from_TransformFilter(tf);
    const WAVEFORMATEX *input_format = NULL;
    const MPEG1WAVEFORMAT *mpeg_format = NULL;
    WAVEFORMATEX *output_format = NULL;

    TRACE("(%p)\n", This);

    if (PINDIR_INPUT != dir)
    {
        return S_OK;
    }

    if (!IsEqualGUID(&pmt->majortype, &MEDIATYPE_Audio))
    {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    if (IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_MPEG1Packet))
    {
        FIXME("MEDIATYPE_MPEG1Packet is not supported!\n");
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    if (!IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_MPEG1AudioPayload) &&
        !IsEqualGUID(&pmt->subtype, &MEDIASUBTYPE_MPEG1Payload))
    {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    if (!IsEqualGUID(&pmt->formattype, &FORMAT_WaveFormatEx) || pmt->cbFormat < sizeof(WAVEFORMATEX))
    {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    input_format = (const WAVEFORMATEX *)pmt->pbFormat;
    if (!input_format)
    {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    if (WAVE_FORMAT_MPEG != input_format->wFormatTag ||
        sizeof(WAVEFORMATEX) + input_format->cbSize < sizeof(MPEG1WAVEFORMAT))
    {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    mpeg_format = (const MPEG1WAVEFORMAT *)input_format;
    if (mpeg_format->fwHeadLayer & ACM_MPEG_LAYER3)
    {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    output_format = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    if (!output_format)
    {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    output_format->wFormatTag = WAVE_FORMAT_PCM;
    output_format->nChannels = input_format->nChannels;
    output_format->nSamplesPerSec = input_format->nSamplesPerSec;
    /* TODO add support for 8-bit output */
    output_format->wBitsPerSample = 16;
    output_format->nBlockAlign = (output_format->nChannels * output_format->wBitsPerSample) / 8;
    output_format->nAvgBytesPerSec = output_format->nSamplesPerSec * output_format->nBlockAlign;
    output_format->cbSize = 0;

    FreeMediaType(&This->tf.pmt);

    This->tf.pmt.majortype = MEDIATYPE_Audio;
    This->tf.pmt.subtype = MEDIASUBTYPE_PCM;
    This->tf.pmt.bFixedSizeSamples = TRUE;
    This->tf.pmt.bTemporalCompression = FALSE;
    This->tf.pmt.lSampleSize = output_format->nBlockAlign;
    This->tf.pmt.formattype = FORMAT_WaveFormatEx;
    This->tf.pmt.pUnk = NULL;
    This->tf.pmt.cbFormat = sizeof(*output_format);
    This->tf.pmt.pbFormat = (BYTE *)output_format;

    This->frame_size = ((mpeg_format->fwHeadLayer & ACM_MPEG_LAYER2)? 1152 : 384) * output_format->nBlockAlign;

    return S_OK;
}

static HRESULT WINAPI MPEGAudioCodec_CompleteConnect(TransformFilter *const tf, const PIN_DIRECTION dir, IPin *const pin)
{
    MPEGAudioCodecImpl *This = impl_from_TransformFilter(tf);
    int error = MPG123_OK;

    TRACE("(%p)\n", This);

    if (PINDIR_INPUT != dir)
    {
        return S_OK;
    }

    This->mh = mpg123_new(NULL, &error);
    if (!This->mh)
    {
        ERR("Unable to create mpg123 handle: %s\n", mpg123_plain_strerror(error));
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    if (MPG123_OK != mpg123_open_feed(This->mh))
    {
        ERR("Unable to open mpg123 bitstream: %s\n", mpg123_strerror(This->mh));
        mpg123_delete(This->mh);
        This->mh = NULL;
        This->frame_size = 0;
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    return S_OK;
}

static HRESULT WINAPI MPEGAudioCodec_BreakConnect(TransformFilter *tf, PIN_DIRECTION dir)
{
    MPEGAudioCodecImpl *This = impl_from_TransformFilter(tf);

    TRACE("(%p)->(%i)\n", This, dir);

    if (PINDIR_INPUT == dir)
    {
        if (This->mh)
        {
            mpg123_close(This->mh);
            mpg123_delete(This->mh);
            This->mh = NULL;
            This->frame_size = 0;
        }
    }

    return S_OK;
}

static HRESULT WINAPI MPEGAudioCodec_DecideBufferSize(TransformFilter *const tf, IMemAllocator *const allocator, ALLOCATOR_PROPERTIES *const input_request)
{
    MPEGAudioCodecImpl *const This = impl_from_TransformFilter(tf);
    ALLOCATOR_PROPERTIES actual;

    TRACE("(%p)\n", This);

    if (!input_request->cbAlign)
    {
        input_request->cbAlign = 1;
    }

    input_request->cbBuffer = max(input_request->cbBuffer, 4 * This->frame_size);

    if (!input_request->cBuffers)
    {
        input_request->cBuffers = 8;
    }

    return IMemAllocator_SetProperties(allocator, input_request, &actual);
}

static const TransformFilterFuncTable MPEGAudioCodec_FuncsTable = {
    MPEGAudioCodec_DecideBufferSize,
    NULL,
    MPEGAudioCodec_Receive,
    NULL,
    NULL,
    MPEGAudioCodec_SetMediaType,
    MPEGAudioCodec_CompleteConnect,
    MPEGAudioCodec_BreakConnect,
    NULL,
    NULL,
    NULL,
    NULL,
};

static const IBaseFilterVtbl MPEGAudioCodec_Vtbl =
{
    TransformFilterImpl_QueryInterface,
    BaseFilterImpl_AddRef,
    TransformFilterImpl_Release,
    BaseFilterImpl_GetClassID,
    TransformFilterImpl_Stop,
    TransformFilterImpl_Pause,
    TransformFilterImpl_Run,
    BaseFilterImpl_GetState,
    BaseFilterImpl_SetSyncSource,
    BaseFilterImpl_GetSyncSource,
    BaseFilterImpl_EnumPins,
    TransformFilterImpl_FindPin,
    BaseFilterImpl_QueryFilterInfo,
    BaseFilterImpl_JoinFilterGraph,
    BaseFilterImpl_QueryVendorInfo,
};

HRESULT MPEGAudioCodec_create(IUnknown *const outer, void **const ppv)
{
    HRESULT hr = S_OK;
    MPEGAudioCodecImpl *This = NULL;

    TRACE("(%p, %p)\n", outer, ppv);

    mpg123_init();

    *ppv = NULL;

    if (outer)
    {
        return CLASS_E_NOAGGREGATION;
    }

    hr = TransformFilter_Construct(&MPEGAudioCodec_Vtbl, sizeof(MPEGAudioCodecImpl), &CLSID_CMpegAudioCodec, &MPEGAudioCodec_FuncsTable, (IBaseFilter **)&This);
    if (FAILED(hr))
    {
        return hr;
    }

    This->mh = NULL;
    This->frame_size = 0;
    *ppv = &This->tf.filter.IBaseFilter_iface;

    return hr;
}
