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

#include "config.h"

#include "SmoothSwitchOnGOP.h"
#include "Logging.h"

namespace WebCore {

void SmoothSwitchOnGOP::handleChangeInAlreadyEnqueuedContent(ChangeInAlreadyEnqueuedContent change)
{
    if (std::holds_alternative<StateCollecting>(m_currentState)) {
        DEBUG_LOG(LOGIDENTIFIER, "Ignoring content change due to already being in collecting state");
        return;
    }

    DEBUG_LOG(LOGIDENTIFIER, "Transferred not yet enqueued samples, will start collecting new frames");
    // TODO Handle edge case: change while vacating. The old samples and vacating queue need to be merged, but what is the correct way to do it?
    // TODO How should the futureEnqueueDiscontinuityBoundary be merged? Max?
    m_currentState = StateCollecting { WTF::move(change.notYetEnqueuedSamples), {}, change.futureEnqueueDiscontinuityBoundary };
}

bool SmoothSwitchOnGOP::isReadyForMoreSamples()
{
    if (std::holds_alternative<StateVacating>(m_currentState))
        return m_innerSink->isReadyForMoreSamples();
    if (std::holds_alternative<StateCollecting>(m_currentState)) {
        // When we're doing a smooth switch we want the SourceBuffer to provide samples ASAP,
        // since our decision on how to finish the smooth switch will depend on them.
        // We will still only feed the inner sink as much as it can handle.
        return true;
    }
    ASSERT_NOT_REACHED();
}

void SmoothSwitchOnGOP::notifyWhenReadyForMoreSamples(std::function<void()>&& callback)
{
    // TODO
    UNUSED_PARAM(callback);
}

void SmoothSwitchOnGOP::enqueueSample(Ref<MediaSample>&& sample)
{
    DEBUG_LOG(LOGIDENTIFIER, "Enqueue with DTS=", sample->decodeTime().toDouble());
    DecodeOrderSampleMap::KeyType decodeKey { sample->decodeTime(), sample->presentationTime() };

    if (auto* current = std::get_if<StateVacating>(&m_currentState))
        current->vacatingQueue.insert({ decodeKey, sample });
    else if (auto* current = std::get_if<StateCollecting>(&m_currentState)) {
        if (sample->isSync())
            current->newGop.clear();
        current->newGop.insert({ decodeKey, sample });

        if (sample->isSync() && (m_lastDeliveredDecodeKey.first.isInvalid() || decodeKey > m_lastDeliveredDecodeKey)) {
            DEBUG_LOG(LOGIDENTIFIER, "Switching to new GOP with DTS=", sample->decodeTime().toDouble());
            current->oldSamples.erase(current->oldSamples.lower_bound(decodeKey), current->oldSamples.end());
            current->oldSamples.insert({ decodeKey, WTF::move(sample) });
            m_currentState = StateVacating { WTF::move(current->oldSamples) };
        } else if (!sample->isSync() && sample->decodeTime() > current->futureEnqueueDiscontinuityBoundary)  {
            DEBUG_LOG(LOGIDENTIFIER, "Switch to collected GOP (expensive case), will need to use non-displaying copies for samples with presentation end <= ", m_highestDeliveredPresentationEndTime.toDouble());
            m_currentState = StateVacating { WTF::move(current->newGop) };
        }
    }

    tryFeedInnerSink();
}

void SmoothSwitchOnGOP::allSamplesEnqueued()
{
    DEBUG_LOG(LOGIDENTIFIER);
    // TODO: how to handle EOS? Probably just a flag, to keep things simple, as the other alternative would be a fake "EOS" MediaSample.
}

void SmoothSwitchOnGOP::deliverSample(Ref<MediaSample>&& sample)
{
    if (m_highestDeliveredPresentationEndTime.isValid() && sample->presentationEndTime() <= m_highestDeliveredPresentationEndTime)
        sample = sample->createNonDisplayingCopy();

    if (m_highestDeliveredPresentationEndTime.isInvalid() || sample->presentationEndTime() > m_highestDeliveredPresentationEndTime)
        m_highestDeliveredPresentationEndTime = sample->presentationEndTime();
    m_lastDeliveredDecodeKey = { sample->decodeTime(), sample->presentationTime() };
    m_innerSink->enqueueSample(WTF::move(sample));
}

void SmoothSwitchOnGOP::flush()
{
    DEBUG_LOG(LOGIDENTIFIER);
    m_innerSink->flush();
    m_lastDeliveredDecodeKey = { MediaTime::invalidTime(), MediaTime::invalidTime() };
    m_highestDeliveredPresentationEndTime = MediaTime::invalidTime();
    m_currentState = StateVacating { };
}

void SmoothSwitchOnGOP::tryFeedInnerSink()
{
    if (auto* current = std::get_if<StateCollecting>(&m_currentState))
        tryFeedInnerSinkSamples(current->oldSamples);
    else if (auto* current = std::get_if<StateVacating>(&m_currentState))
        tryFeedInnerSinkSamples(current->vacatingQueue);
    else
        ASSERT_NOT_REACHED();
}

void SmoothSwitchOnGOP::tryFeedInnerSinkSamples(DecodeOrderSampleMap::MapType& samples)
{
    while (!samples.empty()) {
        if (!m_innerSink->isReadyForMoreSamples())
            return;
        Ref<MediaSample> sample = samples.begin()->second;
        samples.erase(samples.begin());
        deliverSample(WTF::move(sample));
    }
}

#if !RELEASE_LOG_DISABLED
WTFLogChannel& SmoothSwitchOnGOP::logChannel() const
{
    return LogMediaSource;
}
#endif

} // namespace WebCore