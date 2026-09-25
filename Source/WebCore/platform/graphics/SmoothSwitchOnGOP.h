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

#include "WebCore/MediaSample.h"
#include "WebCore/SampleMap.h"
#include "wtf/AbstractRefCounted.h"

namespace WebCore {

class SampleSink : public AbstractRefCounted {
public:
    virtual ~SampleSink() {}
    virtual bool isReadyForMoreSamples() = 0;
    virtual void notifyWhenReadyForMoreSamples(std::function<void()>&& callback) = 0;
    virtual void enqueueSample(Ref<MediaSample>&&) = 0;
    virtual void allSamplesEnqueued() = 0;
    virtual void flush() = 0;
    virtual void handleChangeInAlreadyEnqueuedContent(DecodeOrderSampleMap::MapType) { flush(); }
};

class SmoothSwitchOnGOP final: public ThreadSafeRefCounted<SmoothSwitchOnGOP>, public SampleSink {
    WTF_MAKE_TZONE_ALLOCATED(SmoothSwitchOnGOP);
public:
    void ref() const final { ThreadSafeRefCounted::ref(); }
    void deref() const final { ThreadSafeRefCounted::deref(); }

    static Ref<SmoothSwitchOnGOP> create(const Ref<SampleSink>& innerSink) { return adoptRef(*new SmoothSwitchOnGOP(innerSink)); }

    bool isReadyForMoreSamples() final;
    void notifyWhenReadyForMoreSamples(std::function<void()>&&) final;
    void enqueueSample(Ref<MediaSample>&&) final;
    void allSamplesEnqueued() final;
    void flush() final;
    void handleChangeInAlreadyEnqueuedContent(DecodeOrderSampleMap::MapType notYetEnqueuedSamples) final;

private:
    SmoothSwitchOnGOP(const Ref<SampleSink>& innerSink)
        : m_innerSink(innerSink)
    {}
    Ref<SampleSink> m_innerSink;

    MediaTime m_highestDeliveredPTS { MediaTime::invalidTime() };
    DecodeOrderSampleMap::KeyType m_lastDeliveredDecodeKey { MediaTime::invalidTime(), MediaTime::invalidTime() };

    DecodeOrderSampleMap::MapType m_oldSamples;
    DecodeOrderSampleMap::MapType m_newSamples;

    void tryFeedInnerSink();
};

} // namespace WebCore
