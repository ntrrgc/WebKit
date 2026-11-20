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
#include <wtf/StringPrintStream.h>
#include <gst/gst.h>

GST_DEBUG_CATEGORY_EXTERN(webkit_media_player_debug);
#define GST_CAT_DEFAULT webkit_media_player_debug

namespace WebCore {

#if !RELEASE_LOG_DISABLED
String sampleMapToRangeString(const DecodeOrderSampleMap::MapType& samples)
{
    if (samples.empty())
        return "[]"_s;
    double start = samples.begin()->first.first.toDouble();
    Ref<MediaSample> lastSample = samples.rbegin()->second;
    double end = (lastSample->decodeTime() + lastSample->duration()).toDouble();
    return makeString("["_s, start, ", "_s, end, "]"_s);
}
#endif

void SmoothSwitchOnGOP::handleChangeInAlreadyEnqueuedContent(ChangeInAlreadyEnqueuedContent change)
{
    DEBUG_LOG(LOGIDENTIFIER, "Change with not yet enqueued samples ", sampleMapToRangeString(change.notYetEnqueuedSamples), ", discontinuity boundary: ", change.futureEnqueueDiscontinuityBoundary.toDouble());
    if (std::holds_alternative<StateCollecting>(m_currentState)) {
        DEBUG_LOG(LOGIDENTIFIER, "Ignoring content change due to already being in collecting state");
        return;
    }

    DEBUG_LOG(LOGIDENTIFIER, "Current vacating queue sample range: ", sampleMapToRangeString(std::get<StateVacating>(m_currentState).vacatingQueue));
    // TODO debugging code, remove later
    const DecodeOrderSampleMap::MapType& vacating = std::get<StateVacating>(m_currentState).vacatingQueue;
    if (!vacating.empty() && !change.notYetEnqueuedSamples.empty()) {
        PlatformTimeRanges vacatingRange(vacating.begin()->first.first, vacating.rbegin()->first.first + protect(vacating.rbegin()->second)->duration());
        PlatformTimeRanges notYetEnqRange(change.notYetEnqueuedSamples.begin()->first.first, change.notYetEnqueuedSamples.rbegin()->first.first + protect(change.notYetEnqueuedSamples.rbegin()->second)->duration());
        vacatingRange.intersectWith(notYetEnqRange);
        if (vacatingRange.length()) {
            DEBUG_LOG(LOGIDENTIFIER, "MIAU SUS OVERLAP!! ", vacatingRange.toString());
            ::abort();
        }
    }

    // TODO Does the order of concatenation (i.e. which set gets priority) matter?
    // So far it doesn't seem so, supporting the hypothesis that the sets are disjoint.
    // Option A:
    StateCollecting newState { WTF::move(std::get<StateVacating>(m_currentState).vacatingQueue), {}, change.futureEnqueueDiscontinuityBoundary };
    newState.oldSamples.insert(change.notYetEnqueuedSamples.begin(), change.notYetEnqueuedSamples.end());
    // Option B:
    // StateCollecting newState { WTF::move(change.notYetEnqueuedSamples), {}, change.futureEnqueueDiscontinuityBoundary };
    // newState.oldSamples.insert(std::get<StateVacating>(m_currentState).vacatingQueue.begin(), std::get<StateVacating>(m_currentState).vacatingQueue.end());

    m_currentState = WTF::move(newState);
}

bool SmoothSwitchOnGOP::isReadyForMoreSamples()
{
    if (std::holds_alternative<StateVacating>(m_currentState))
        return m_innerSink->isReadyForMoreSamples();
    ASSERT(std::holds_alternative<StateCollecting>(m_currentState));
    // When we're doing a smooth switch we want the SourceBuffer to provide samples ASAP,
    // since our decision on how to finish the smooth switch will depend on them.
    // We will still only feed the inner sink as much as it can handle.
    return true;
}

void SmoothSwitchOnGOP::notifyWhenReadyForMoreSamples(std::function<void()>&& callback)
{
    // In Collecting state, we always report being ready for more samples, so the producer
    // would only get a negative isReadyForMoreSamples() in Vacating state.
    ASSERT(std::holds_alternative<StateVacating>(m_currentState));

    m_upstreamReadyForMoreSamplesCallback = WTF::move(callback);
    hookDownstreamReadyForMoreSamples();
}

static DecodeOrderSampleMap::const_iterator findSampleAfterLastDisplayable(const DecodeOrderSampleMap::MapType& samples, MediaTime highestDeliveredPresentationEndTime)
{
    // If no sample is displayable, we consider the last displayable sample was before the start of the map.
    // This is consistent with the use case for this function: we want to delete samples after the last displayable
    // because they will be redundant, if no samples are displayable, they all are redundant.
    auto sampleAfterLastDisplayable = samples.begin();
    auto it = samples.begin();
    for (; it != samples.end(); ++it) {
        Ref<MediaSample> sample = it->second;
        bool isDisplayable = highestDeliveredPresentationEndTime.isInvalid() || sample->presentationEndTime() > highestDeliveredPresentationEndTime;
        if (isDisplayable)
            sampleAfterLastDisplayable = std::next(it);

        if (highestDeliveredPresentationEndTime.isInvalid() || sample->presentationEndTime() > highestDeliveredPresentationEndTime)
            highestDeliveredPresentationEndTime = sample->presentationEndTime();
    }
    return sampleAfterLastDisplayable;
}

void SmoothSwitchOnGOP::enqueueSample(Ref<MediaSample>&& sample)
{
    DecodeOrderSampleMap::KeyType decodeKey { sample->decodeTime(), sample->presentationTime() };

    if (auto* current = std::get_if<StateVacating>(&m_currentState)) {
        DEBUG_LOG(LOGIDENTIFIER, "Inserting in vacatingQueue: ", toString(sample.get()));
        current->vacatingQueue.insert({ decodeKey, sample });
    } else if (auto* current = std::get_if<StateCollecting>(&m_currentState)) {
        if (sample->isSync()) {
            DEBUG_LOG(LOGIDENTIFIER, "Cleared newGop.");
            current->newGop.clear();
        }
        DEBUG_LOG(LOGIDENTIFIER, "Inserting in newGop: ", toString(sample.get()));
        current->newGop.insert({ decodeKey, sample });

        if (sample->isSync() && (m_lastDeliveredDecodeKey.first.isInvalid() || decodeKey > m_lastDeliveredDecodeKey)) {
            DEBUG_LOG(LOGIDENTIFIER, "Switching to new GOP with DTS=", sample->decodeTime().toDouble());
            DecodeOrderSampleMap::MapType vacatingQueue = WTF::move(current->oldSamples);
            // Anything with later DTS than the new GOP can be safely deleted.
            vacatingQueue.erase(vacatingQueue.lower_bound(decodeKey), vacatingQueue.end());
            // Everything after the last displayable sample in oldSamples is redundant and can be deleted.
            // (See DTS=3 of generation 2 in media-source-smooth-switch-twice.html for an example)
            vacatingQueue.erase(findSampleAfterLastDisplayable(vacatingQueue, m_highestDeliveredPresentationEndTime), vacatingQueue.end());
            vacatingQueue.insert({ decodeKey, WTF::move(sample) });
            m_currentState = StateVacating { WTF::move(vacatingQueue) };
        } else if (!sample->isSync() && sample->decodeTime() > current->futureEnqueueDiscontinuityBoundary)  {
            DEBUG_LOG(LOGIDENTIFIER, "Switch to collected GOP (expensive case), will need to use non-displaying copies for samples with presentation end <= ", m_highestDeliveredPresentationEndTime.toDouble());
            m_currentState = StateVacating { WTF::move(current->newGop) };
        }
    }

    tryFeedInnerSink();
}

void SmoothSwitchOnGOP::allSamplesEnqueued()
{
    DEBUG_LOG(LOGIDENTIFIER, "All samples have been enqueued");
    m_hasAllSamples = true;

    if (!m_hasDeliveredEndOfStream && isEmpty()) {
        m_innerSink->allSamplesEnqueued();
        m_hasDeliveredEndOfStream = true;
    }
}

bool SmoothSwitchOnGOP::isEmpty()
{
    if (auto* current = std::get_if<StateCollecting>(&m_currentState))
        return current->oldSamples.empty();
    else if (auto* current = std::get_if<StateVacating>(&m_currentState))
        return current->vacatingQueue.empty();

    ASSERT_NOT_REACHED();
    return false;
}

void SmoothSwitchOnGOP::deliverSample(Ref<MediaSample>&& sample)
{
    if (m_highestDeliveredPresentationEndTime.isValid() && sample->presentationEndTime() <= m_highestDeliveredPresentationEndTime)
        sample = sample->createNonDisplayingCopy();

    if (m_highestDeliveredPresentationEndTime.isInvalid() || sample->presentationEndTime() > m_highestDeliveredPresentationEndTime)
        m_highestDeliveredPresentationEndTime = sample->presentationEndTime();
    m_lastDeliveredDecodeKey = { sample->decodeTime(), sample->presentationTime() };
    DEBUG_LOG(LOGIDENTIFIER, "Delivering sample to inner sink: ", toString(sample));
    m_innerSink->enqueueSample(WTF::move(sample));
}

void SmoothSwitchOnGOP::flush()
{
    DEBUG_LOG(LOGIDENTIFIER);
    m_innerSink->flush();
    m_lastDeliveredDecodeKey = { MediaTime::invalidTime(), MediaTime::invalidTime() };
    m_highestDeliveredPresentationEndTime = MediaTime::invalidTime();
    m_currentState = StateVacating { };
    m_hasAllSamples = false;
    m_hasDeliveredEndOfStream = false;
}

void SmoothSwitchOnGOP::tryFeedInnerSink()
{
    if (auto* current = std::get_if<StateCollecting>(&m_currentState)) {
        DEBUG_LOG(LOGIDENTIFIER, "Feeding inner sink, current state: collecting");
        tryFeedInnerSinkSamples(current->oldSamples);
    } else if (auto* current = std::get_if<StateVacating>(&m_currentState)) {
        DEBUG_LOG(LOGIDENTIFIER, "Feeding inner sink, current state: vacating");
        tryFeedInnerSinkSamples(current->vacatingQueue);
    } else
        ASSERT_NOT_REACHED();
}

// TODO move above tryFeedInnerSink()
void SmoothSwitchOnGOP::innerSinkNotifiedReadyForMoreSamples()
{
    DEBUG_LOG(LOGIDENTIFIER, "Inner sink reports being now ready for more samples.");
    tryFeedInnerSink();
    if (m_innerSink->isReadyForMoreSamples()) {
        // The inner sink could accept all samples we had buffered, so further notify upstream to give us more.
        if (std::function<void()> callback = std::exchange(m_upstreamReadyForMoreSamplesCallback, std::function<void()>()))
            callback();
    }
}

void SmoothSwitchOnGOP::tryFeedInnerSinkSamples(DecodeOrderSampleMap::MapType& samples)
{
    DEBUG_LOG(LOGIDENTIFIER, samples.size(), " samples available to feed the inner sink.");
    DEBUG_LOG(LOGIDENTIFIER, "MIAU 1.");
    while (!samples.empty() && m_innerSink->isReadyForMoreSamples()) {
        Ref<MediaSample> sample = samples.begin()->second;
        samples.erase(samples.begin());
        deliverSample(WTF::move(sample));
    }
    DEBUG_LOG(LOGIDENTIFIER, "MIAU 2.");

    if (!m_hasDeliveredEndOfStream && m_hasAllSamples && samples.empty()) {
        DEBUG_LOG(LOGIDENTIFIER, "No samples left and m_hasAllSamples set, delivering allSamplesEnqueued() notification to the inner sink.");
        m_innerSink->allSamplesEnqueued();
        m_hasDeliveredEndOfStream = true;
        return;
    }
    DEBUG_LOG(LOGIDENTIFIER, "MIAU 3.");

    // SBP may not have the need to enqueue anything more while still have samples left because the inner sink was not ready:
    // we still want those samples to be delivered.
    if (!samples.empty() && !m_innerSink->isReadyForMoreSamples())
        hookDownstreamReadyForMoreSamples();
}

void SmoothSwitchOnGOP::hookDownstreamReadyForMoreSamples()
{
    m_innerSink->notifyWhenReadyForMoreSamples([weakThis = ThreadSafeWeakPtr { *this }] {
        if (RefPtr<SmoothSwitchOnGOP> strongThis = weakThis.get())
            strongThis->innerSinkNotifiedReadyForMoreSamples();
    });
}

#if !RELEASE_LOG_DISABLED
WTFLogChannel& SmoothSwitchOnGOP::logChannel() const
{
    return LogMediaSource;
}
#endif

} // namespace WebCore