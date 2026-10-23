/*
 * Copyright (C) 2026 Igalia, S.L.
 * Copyright (C) 2026 Comcast
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <variant>
#include "WebCore/MediaSample.h"
#include "WebCore/SampleMap.h"
#include "WebCore/SourceBufferPrivate.h"
#include "wtf/AbstractRefCounted.h"
#include "wtf/LoggerHelper.h"

namespace WebCore {

class SampleSink : public AbstractRefCounted {
public:
    virtual ~SampleSink() {}
    virtual bool isReadyForMoreSamples() = 0;
    virtual void notifyWhenReadyForMoreSamples(std::function<void()>&& callback) = 0;
    virtual void enqueueSample(Ref<MediaSample>&&) = 0;
    virtual void allSamplesEnqueued() = 0;
    virtual void flush() = 0;
    virtual void handleChangeInAlreadyEnqueuedContent(ChangeInAlreadyEnqueuedContent) {
        flush();
    }
};

class SmoothSwitchOnGOP final
    : public ThreadSafeRefCounted<SmoothSwitchOnGOP>
    , public SampleSink
#if !RELEASE_LOG_DISABLED
    , public LoggerHelper
#endif
{
    WTF_MAKE_TZONE_ALLOCATED(SmoothSwitchOnGOP);
public:
    void ref() const final { ThreadSafeRefCounted::ref(); }
    void deref() const final { ThreadSafeRefCounted::deref(); }

    static Ref<SmoothSwitchOnGOP> create(SourceBufferPrivate& sourceBufferPrivate, const Ref<SampleSink>& innerSink) { return adoptRef(*new SmoothSwitchOnGOP(sourceBufferPrivate, innerSink)); }

    bool isReadyForMoreSamples() final;
    void notifyWhenReadyForMoreSamples(std::function<void()>&&) final;
    void enqueueSample(Ref<MediaSample>&&) final;
    void allSamplesEnqueued() final;
    void flush() final;
    void handleChangeInAlreadyEnqueuedContent(ChangeInAlreadyEnqueuedContent) final;

private:
    SmoothSwitchOnGOP(SourceBufferPrivate& sourceBufferPrivate, const Ref<SampleSink>& innerSink)
        : m_innerSink(innerSink)
#if !RELEASE_LOG_DISABLED
        , m_logger(sourceBufferPrivate.sourceBufferLogger())
        , m_logIdentifier(sourceBufferPrivate.sourceBufferLogIdentifier())
#endif
    {}
    Ref<SampleSink> m_innerSink;

    MediaTime m_highestDeliveredPresentationEndTime { MediaTime::invalidTime() };
    DecodeOrderSampleMap::KeyType m_lastDeliveredDecodeKey { MediaTime::invalidTime(), MediaTime::invalidTime() };

    struct StateCollecting {
        DecodeOrderSampleMap::MapType oldSamples;
        DecodeOrderSampleMap::MapType newGop;
        MediaTime futureEnqueueDiscontinuityBoundary;
    };
    struct StateVacating {
        DecodeOrderSampleMap::MapType vacatingQueue;
    };
    typedef std::variant<StateCollecting, StateVacating> State;
    State m_currentState { StateVacating { } };

    void deliverSample(Ref<MediaSample>&&);
    void tryFeedInnerSink();
    void tryFeedInnerSinkSamples(DecodeOrderSampleMap::MapType&);

#if !RELEASE_LOG_DISABLED
    const Logger& logger() const final { return m_logger; }
    uint64_t logIdentifier() const final { return m_logIdentifier; }
    ASCIILiteral logClassName() const { return "SmoothSwitchOnGOP"_s; }
    WTFLogChannel& NODELETE logChannel() const;

    const Ref<const Logger> m_logger;
    const uint64_t m_logIdentifier;
#endif
};

} // namespace WebCore
