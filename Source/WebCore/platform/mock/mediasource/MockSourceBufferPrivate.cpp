/*
 * Copyright (C) 2013-2017 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "MockSourceBufferPrivate.h"

#if ENABLE(MEDIA_SOURCE)

#include "Logging.h"
#include "MediaDescription.h"
#include "MediaPlayer.h"
#include "MediaSample.h"
#include "MockBox.h"
#include "MockMediaPlayerMediaSource.h"
#include "MockMediaSourcePrivate.h"
#include "MockTracks.h"
#include "SharedBuffer.h"
#include "SourceBufferPrivateClient.h"
#include <JavaScriptCore/ArrayBuffer.h>
#include <wtf/NativePromise.h>
#include <wtf/StringPrintStream.h>

namespace WebCore {

class MockMediaSample final : public MediaSample {
public:
    static Ref<MockMediaSample> create(const MockSampleBox& box) { return adoptRef(*new MockMediaSample(box)); }
    virtual ~MockMediaSample() = default;

private:
    MockMediaSample(const MockSampleBox& box)
        : m_box(box)
        , m_id(box.trackID())
    {
    }

    MediaTime presentationTime() const override { return m_box.presentationTimestamp(); }
    MediaTime decodeTime() const override { return m_box.decodeTimestamp(); }
    MediaTime duration() const override { return m_box.duration(); }
    TrackID trackID() const override { return m_id; }
    size_t sizeInBytes() const override { return sizeof(m_box); }
    SampleFlags flags() const override;
    PlatformSample platformSample() const override;
    Type type() const override { return Type::MockSampleBox; }
    FloatSize presentationSize() const override { return FloatSize(); }
    void dump(PrintStream&) const override;
    void offsetTimestampsBy(const MediaTime& offset) override { m_box.offsetTimestampsBy(offset); }
    void setTimestamps(const MediaTime& presentationTimestamp, const MediaTime& decodeTimestamp) override { m_box.setTimestamps(presentationTimestamp, decodeTimestamp); }
    Ref<MediaSample> createNonDisplayingCopy() const override;
    Ref<MediaSample> createCopyWithAdjustedStartTime(const MediaTime& offset) const override;

    unsigned NODELETE generation() const { return m_box.generation(); }

    MockSampleBox m_box;
    TrackID m_id;
};

MediaSample::SampleFlags MockMediaSample::flags() const
{
    unsigned flags = None;
    if (m_box.isSync())
        flags |= IsSync;
    if (m_box.isNonDisplaying())
        flags |= IsNonDisplaying;
    return SampleFlags(flags);
}

PlatformSample MockMediaSample::platformSample() const
{
    return PlatformSample { &m_box };
}

void MockMediaSample::dump(PrintStream& out) const
{
    out.print("{PTS(", presentationTime(), "), DTS(", decodeTime(), "), duration(", duration(), "), flags(", (int)flags(), "), generation(", generation(), ")}");
}

Ref<MediaSample> MockMediaSample::createNonDisplayingCopy() const
{
    auto copy = MockMediaSample::create(m_box);
    copy->m_box.setFlag(MockSampleBox::IsNonDisplaying);
    return copy;
}

Ref<MediaSample> MockMediaSample::createCopyWithAdjustedStartTime(const MediaTime& offset) const
{
    MediaTime clampedOffset = std::max(MediaTime::zeroTime(), std::min(offset, m_box.duration()));

    auto copy = MockMediaSample::create(m_box);
    copy->m_box.setTimestamps(m_box.presentationTimestamp() + clampedOffset, m_box.decodeTimestamp() + clampedOffset);
    copy->m_box.setDuration(m_box.duration() - clampedOffset);
    return copy;
}

class MockMediaDescription final : public MediaDescription {
public:
    static Ref<MockMediaDescription> create(const MockTrackBox& box) { return adoptRef(*new MockMediaDescription(box)); }
    virtual ~MockMediaDescription() = default;

    bool isVideo() const final { return m_box.kind() == MockTrackBox::Video; }
    bool isAudio() const final { return m_box.kind() == MockTrackBox::Audio; }
    bool isText() const final { return m_box.kind() == MockTrackBox::Text; }

private:
    MockMediaDescription(const MockTrackBox& box)
        : MediaDescription(box.codec().isolatedCopy())
        , m_box(box)
    {
    }
    const MockTrackBox m_box;
};

Ref<MockSourceBufferPrivate> MockSourceBufferPrivate::create(MockMediaSourcePrivate& parent)
{
    return adoptRef(*new MockSourceBufferPrivate(parent));
}

MockSourceBufferPrivate::MockSourceBufferPrivate(MockMediaSourcePrivate& parent)
    : SourceBufferPrivate(parent)
#if !RELEASE_LOG_DISABLED
    , m_logger(parent.logger())
    , m_logIdentifier(parent.nextSourceBufferLogIdentifier())
#endif
{
}

MockSourceBufferPrivate::~MockSourceBufferPrivate() = default;

RefPtr<MockMediaSourcePrivate> MockSourceBufferPrivate::mediaSourcePrivate() const
{
    return dynamicDowncast<MockMediaSourcePrivate>(m_mediaSource.get());
}

Ref<MediaPromise> MockSourceBufferPrivate::appendInternal(Ref<SharedBuffer>&& data)
{
    m_inputBuffer.appendVector(data->extractData());

    while (m_inputBuffer.size()) {
        auto buffer = ArrayBuffer::create(m_inputBuffer);
        uint64_t boxLength = MockBox::peekLength(buffer.ptr());
        if (boxLength > buffer->byteLength())
            break;

        String type = MockBox::peekType(buffer.ptr());
        if (type == MockInitializationBox::type()) {
            MockInitializationBox initBox = MockInitializationBox(buffer.ptr());
            didReceiveInitializationSegment(initBox);
        } else if (type == MockSampleBox::type()) {
            MockSampleBox sampleBox = MockSampleBox(buffer.ptr());
            didReceiveSample(sampleBox);
        } else {
            m_inputBuffer.clear();
            return MediaPromise::createAndReject(PlatformMediaError::ParsingError);
        }
        m_inputBuffer.removeAt(0, boxLength);
    }

    return MediaPromise::createAndResolve();
}

MockSourceBufferPrivate::MockTrackSampleSink::MockTrackSampleSink(MockSourceBufferPrivate& priv, MockTrack& track, TrackID trackID)
    : m_trackID(trackID)
    , m_track(track)
    , m_private(priv)
{
}

MockSourceBufferPrivate::MockTrack::MockTrack(MockSourceBufferPrivate& priv, TrackID trackID)
    : innerSink(MockTrackSampleSink::create(priv, *this, trackID))
    , activeSink(innerSink)
{
}

bool MockSourceBufferPrivate::isReadyForMoreSamples(TrackID trackID)
{
    return m_tracks.at(trackID)->activeSink->isReadyForMoreSamples();
}

bool MockSourceBufferPrivate::MockTrackSampleSink::isReadyForMoreSamples()
{
    RefPtr<MockTrack> track = m_track.get();
    return !track->maxQueueDepth || track->enqueuedSamples.size() < track->maxQueueDepth.value();
}

void MockSourceBufferPrivate::flush(TrackID trackID)
{
    m_tracks.at(trackID)->activeSink->flush();
}

void MockSourceBufferPrivate::MockTrackSampleSink::flush()
{
    RefPtr<MockTrack> track = m_track.get();
    track->enqueuedSamples.clear();
}

void MockSourceBufferPrivate::didReceiveInitializationSegment(const MockInitializationBox& initBox)
{
    SourceBufferPrivateClient::InitializationSegment segment;
    segment.duration = initBox.duration();

    for (auto& trackBox : initBox.tracks()) {
        TrackID trackID = trackBox.trackID();
        if (!m_tracks.contains(trackID))
            m_tracks.emplace(trackID, MockTrack::create(*this, trackID));

        if (trackBox.kind() == MockTrackBox::Video) {
            SourceBufferPrivateClient::InitializationSegment::VideoTrackInformation info;
            info.track = MockVideoTrackPrivate::create(trackBox);
            info.description = MockMediaDescription::create(trackBox);
            segment.videoTracks.append(info);
        } else if (trackBox.kind() == MockTrackBox::Audio) {
            SourceBufferPrivateClient::InitializationSegment::AudioTrackInformation info;
            info.track = MockAudioTrackPrivate::create(trackBox);
            info.description = MockMediaDescription::create(trackBox);
            segment.audioTracks.append(info);
        } else if (trackBox.kind() == MockTrackBox::Text) {
            SourceBufferPrivateClient::InitializationSegment::TextTrackInformation info;
            info.track = MockTextTrackPrivate::create(trackBox);
            info.description = MockMediaDescription::create(trackBox);
            segment.textTracks.append(info);
        }
    }

    SourceBufferPrivate::didReceiveInitializationSegment(WTF::move(segment));
}

void MockSourceBufferPrivate::didReceiveSample(const MockSampleBox& sampleBox)
{
    SourceBufferPrivate::didReceiveSample(MockMediaSample::create(sampleBox));
}

void MockSourceBufferPrivate::resetParserStateInternal()
{
}

Ref<SourceBufferPrivate::SamplesPromise> MockSourceBufferPrivate::enqueuedSamplesForTrackID(TrackID trackID)
{
    RefPtr<MockTrack> track = m_tracks.at(trackID);
    return SamplesPromise::createAndResolve(copyToVector(track->enqueuedSamples));
}

void MockSourceBufferPrivate::setMaximumQueueDepthForTrackID(TrackID trackID, uint64_t maxQueueDepth)
{
    RefPtr<MockTrack> track = m_tracks.at(trackID);
    track->maxQueueDepth = maxQueueDepth;
    // After the change we may be ready for more samples.
    provideMediaData(trackID);
}

void MockSourceBufferPrivate::setSmoothSwitchStrategyForTrackID(TrackID trackID, const AtomString& strategy)
{
    // Currently the only supported option is "gop" (SmoothSwitchOnGOP).
    // By default (if this method is never called), no smooth switch strategy is applied.
    if (strategy != "gop")
        return; // Unsupported strategy.
    RefPtr<MockTrack> track = m_tracks.at(trackID);
    if (track->smoothSwitchStrategy == SmoothSwitchStrategy::GOP)
        return; // Already configured
    track->smoothSwitchStrategy = SmoothSwitchStrategy::GOP;
    track->activeSink = SmoothSwitchOnGOP::create(track->innerSink);
}

bool MockSourceBufferPrivate::canSetMinimumUpcomingPresentationTime(TrackID) const
{
    return true;
}

bool MockSourceBufferPrivate::canSwitchToType(const ContentType& contentType)
{
    MediaEngineSupportParameters parameters {
        .platformType = PlatformMediaDecodingType::MediaSource,
        .type = contentType
    };
    return MockMediaPlayerMediaSource::supportsType(parameters) != MediaPlayer::SupportsType::IsNotSupported;
}

void MockSourceBufferPrivate::enqueueSample(Ref<MediaSample>&& sample, TrackID trackID)
{
    RefPtr<MockTrack> track = m_tracks.at(trackID);
    track->activeSink->enqueueSample(std::move(sample));
}

void MockSourceBufferPrivate::MockTrackSampleSink::enqueueSample(Ref<MediaSample>&& sample)
{
    RefPtr sourceBufferPrivate = m_private.get();
    if (!sourceBufferPrivate)
        return;
    RefPtr mediaSource = sourceBufferPrivate->mediaSourcePrivate();
    if (!mediaSource)
        return;

    if (sample->type() != MediaSample::Type::MockSampleBox)
        return;

    auto* box = sample->platformSample().mockSampleBox();
    if (!box)
        return;

    mediaSource->incrementTotalVideoFrames();
    if (box->isCorrupted())
        mediaSource->incrementCorruptedFrames();
    if (box->isDropped())
        mediaSource->incrementDroppedFrames();
    if (box->isDelayed())
        mediaSource->incrementTotalFrameDelayBy(MediaTime(1, 1));

    RefPtr<MockTrack> track = sourceBufferPrivate->m_tracks.at(m_trackID);
    track->enqueuedSamples.append(toString(sample.get()));
}

#if !RELEASE_LOG_DISABLED
WTFLogChannel& MockSourceBufferPrivate::logChannel() const
{
    return LogMediaSource;
}
#endif

}

#endif

