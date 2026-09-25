/*
 * Copyright (C) 2013-2025 Apple Inc. All rights reserved.
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

#pragma once

#if ENABLE(MEDIA_SOURCE)

#include "SourceBufferPrivate.h"
#include "SmoothSwitchOnGOP.h"

namespace WebCore {

class AudioTrackPrivate;
class InbandTextTrackPrivate;
class MockInitializationBox;
class MockMediaSourcePrivate;
class MockSampleBox;
class TimeRanges;
class VideoTrackPrivate;

enum class SmoothSwitchStrategy {
    None,
    GOP
};

class MockSourceBufferPrivate final : public SourceBufferPrivate {
public:
    static Ref<MockSourceBufferPrivate> create(MockMediaSourcePrivate&);
    virtual ~MockSourceBufferPrivate();

    constexpr MediaPlatformType platformType() const final { return MediaPlatformType::Mock; }
private:
    struct MockTrack;

    class MockTrackSampleSink final : public SampleSink, public ThreadSafeRefCounted<MockTrackSampleSink> {
    public:
        void ref() const final { ThreadSafeRefCounted::ref(); }
        void deref() const final { ThreadSafeRefCounted::deref(); }
        static Ref<MockTrackSampleSink> create(MockSourceBufferPrivate& priv, MockTrack& track, TrackID trackID) { return adoptRef(*new MockTrackSampleSink(priv, track, trackID)); }

        bool isReadyForMoreSamples() final;
        void notifyWhenReadyForMoreSamples(std::function<void()>&&) final {}
        void enqueueSample(Ref<MediaSample>&&) final;
        void allSamplesEnqueued() final {}
        void flush() final;

    private:
        MockTrackSampleSink(MockSourceBufferPrivate&, MockTrack&, TrackID);

        TrackID m_trackID;
        ThreadSafeWeakPtr<MockTrack> m_track;
        ThreadSafeWeakPtr<MockSourceBufferPrivate> m_private;
    };

    struct MockTrack final : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<MockTrack> {
        static Ref<MockTrack> create(MockSourceBufferPrivate& priv, TrackID trackID) { return adoptRef(*new MockTrack(priv, trackID)); }

        std::optional<uint64_t> maxQueueDepth { std::nullopt };
        Vector<String> enqueuedSamples;

        Ref<MockTrackSampleSink> innerSink;
        Ref<SampleSink> activeSink;
        SmoothSwitchStrategy smoothSwitchStrategy;
    private:
        MockTrack(MockSourceBufferPrivate&, TrackID);
    };

    explicit MockSourceBufferPrivate(MockMediaSourcePrivate&);
    RefPtr<MockMediaSourcePrivate> mediaSourcePrivate() const;

    // SourceBufferPrivate overrides
    Ref<MediaPromise> appendInternal(Ref<SharedBuffer>&&) final;
    void resetParserStateInternal() final;
    bool canSetMinimumUpcomingPresentationTime(TrackID) const final;
    bool canSwitchToType(const ContentType&) final;

    void flush(TrackID) final;
    void enqueueSample(Ref<MediaSample>&&, TrackID) final;
    bool isReadyForMoreSamples(TrackID) final;

    Ref<SamplesPromise> enqueuedSamplesForTrackID(TrackID) final;
    void setMaximumQueueDepthForTrackID(TrackID, uint64_t) final;
    void setSmoothSwitchStrategyForTrackID(TrackID, const AtomString&) final;

    void didReceiveInitializationSegment(const MockInitializationBox&);
    void didReceiveSample(const MockSampleBox&);

#if !RELEASE_LOG_DISABLED
    const Logger& logger() const final { return m_logger.get(); }
    ASCIILiteral logClassName() const override { return "MockSourceBufferPrivate"_s; }
    uint64_t logIdentifier() const final { return m_logIdentifier; }
    WTFLogChannel& logChannel() const final;

    const Logger& sourceBufferLogger() const final { return m_logger.get(); }
    uint64_t sourceBufferLogIdentifier() final { return logIdentifier(); }
#endif

    StdUnorderedMap<TrackID, RefPtr<MockTrack>> m_tracks;
    Vector<uint8_t> m_inputBuffer;

#if !RELEASE_LOG_DISABLED
    const Ref<const Logger> m_logger;
    const uint64_t m_logIdentifier;
#endif
};

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE) && USE(AVFOUNDATION)
