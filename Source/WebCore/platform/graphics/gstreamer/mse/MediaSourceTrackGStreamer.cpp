/*
 * Copyright (C) 2020, 2021 Metrological Group B.V.
 * Copyright (C) 2020, 2021 Igalia, S.L
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "MediaSourceTrackGStreamer.h"

#include "MediaPlayerPrivateGStreamerMSE.h"
#include "MediaSourcePrivateGStreamer.h"
#include "WebKitMediaSourceGStreamer.h"

#if ENABLE(MEDIA_SOURCE) && USE(GSTREAMER)

GST_DEBUG_CATEGORY_STATIC(webkit_mse_track_debug);
#define GST_CAT_DEFAULT webkit_mse_track_debug

namespace WebCore {

MediaSourceTrackGStreamer::MediaSourceTrackGStreamer(GStreamerTrackType type, TrackID trackId, GRefPtr<GstCaps>&& initialCaps, Ref<SourceBufferPrivate> sourceBuffer, RefPtr<MediaSourcePrivateGStreamer> mediaSource, RefPtr<MediaPlayerPrivateGStreamerMSE> player)
    : m_type(type)
    , m_id(trackId)
    , m_initialCaps(WTF::move(initialCaps))
    , m_innerSink(GStreamerTrackSampleSink::create(trackId, type, mediaSource, player))
    , m_activeSink(m_innerSink)
{
    ASSERT(isMainThread());
    static std::once_flag debugRegisteredFlag;
    std::call_once(debugRegisteredFlag, [] {
        GST_DEBUG_CATEGORY_INIT(webkit_mse_track_debug, "webkitmsetrack", 0, "WebKit MSE Track");
    });
    // Activate SmoothSwitchOnGOP:
    m_activeSink = SmoothSwitchOnGOP::create(sourceBuffer, m_innerSink);
}

MediaSourceTrackGStreamer::~MediaSourceTrackGStreamer()
{
    ASSERT(m_isRemoved);
}

Ref<MediaSourceTrackGStreamer::GStreamerTrackSampleSink> MediaSourceTrackGStreamer::GStreamerTrackSampleSink::create(TrackID trackId, GStreamerTrackType trackType, RefPtr<MediaSourcePrivateGStreamer> mediaSource, RefPtr<MediaPlayerPrivateGStreamerMSE> player)
{
    return adoptRef(*new MediaSourceTrackGStreamer::GStreamerTrackSampleSink(trackId, trackType, WTF::move(mediaSource), WTF::move(player)));
}

Ref<MediaSourceTrackGStreamer> MediaSourceTrackGStreamer::create(GStreamerTrackType type, TrackID trackId, GRefPtr<GstCaps>&& initialCaps, Ref<SourceBufferPrivate> sourceBuffer, RefPtr<MediaSourcePrivateGStreamer> mediaSource, RefPtr<MediaPlayerPrivateGStreamerMSE> player)
{
    return adoptRef(*new MediaSourceTrackGStreamer(type, trackId, WTF::move(initialCaps), WTF::move(sourceBuffer), WTF::move(mediaSource), WTF::move(player)));
}

bool MediaSourceTrackGStreamer::GStreamerTrackSampleSink::isReadyForMoreSamples()
{
    ASSERT(isMainThread());
    DataMutexLocker queue { m_queueDataMutex };
    return !queue->isFull();
}

void MediaSourceTrackGStreamer::GStreamerTrackSampleSink::notifyWhenReadyForMoreSamples(TrackQueue::LowLevelHandler&& handler)
{
    ASSERT(isMainThread());
    DataMutexLocker queue { m_queueDataMutex };
    queue->notifyWhenLowLevel([handler = WTF::move(handler)] mutable {
        RunLoop::mainSingleton().dispatch(WTF::move(handler));
    });
}

void MediaSourceTrackGStreamer::GStreamerTrackSampleSink::enqueueObject(GRefPtr<GstMiniObject>&& object)
{
    ASSERT(isMainThread());
    DataMutexLocker queue { m_queueDataMutex };
    queue->enqueueObject(WTF::move(object));
}

void MediaSourceTrackGStreamer::GStreamerTrackSampleSink::enqueueSample(Ref<MediaSample>&& sample)
{
    ASSERT(isMainThread());
    GRefPtr<GstSample> gstSample = sample->platformSample().gstSample();
    ASSERT(gstSample);
    enqueueObject(adoptGRef(GST_MINI_OBJECT(gstSample.leakRef())));
}

void MediaSourceTrackGStreamer::GStreamerTrackSampleSink::allSamplesEnqueued()
{
    ASSERT(isMainThread());
    enqueueObject(adoptGRef(GST_MINI_OBJECT(gst_event_new_eos())));
}

void MediaSourceTrackGStreamer::GStreamerTrackSampleSink::flush()
{
    ASSERT(isMainThread());

    RefPtr<MediaSourcePrivateGStreamer> mediaSource = m_mediaSource.get();
    RefPtr<MediaPlayerPrivateGStreamerMSE> player = m_player.get();
    if (!mediaSource)
        return;

    if (!mediaSource->hasAllTracks()) {
        if (player)
            GST_DEBUG_OBJECT(player->pipeline(), "Source element has not emitted tracks yet, so we only need to clear the queue. trackId = '%" PRIu64 "'", m_trackId);
        DataMutexLocker queue { queueDataMutex() };
        queue->clear();
        return;
    }

    if (m_trackType == GStreamerTrackType::Text) {
        if (player)
            GST_DEBUG_OBJECT(player->pipeline(), "Track is a text stream, so we only need to clear the queue. trackId = '%" PRIu64 "'", m_trackId);
        DataMutexLocker queue { queueDataMutex() };
        queue->clear();
        return;
    }

    if (!player)
        return;
    GST_DEBUG_OBJECT(player->pipeline(), "Source element has emitted tracks, let it handle the flush, which may cause a pipeline flush as well. trackId = '%" PRIu64 "'", m_trackId);
    if (auto source = player->webKitMediaSrc())
        webKitMediaSrcFlush(source, m_trackId);
}

void MediaSourceTrackGStreamer::clearQueue()
{
    ASSERT(isMainThread());
    DataMutexLocker queue { queueDataMutex() };
    queue->clear();
}

void MediaSourceTrackGStreamer::remove()
{
    ASSERT(isMainThread());
    m_isRemoved = true;
}

#undef GST_CAT_DEFAULT

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE) && USE(GSTREAMER)
