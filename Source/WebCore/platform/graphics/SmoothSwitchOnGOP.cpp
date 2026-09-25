#include "SmoothSwitchOnGOP.h"

namespace WebCore {

bool SmoothSwitchOnGOP::isReadyForMoreSamples()
{
    if (!m_oldSamples.empty()) {
        // When we're doing a smooth switch we want the SourceBuffer to provide samples ASAP,
        // since our decision on how to finish the smooth switch will depend on them.
        // We will still only feed the inner sink as much as it can handle.
        return true;
    }
    return m_innerSink->isReadyForMoreSamples();
}

void SmoothSwitchOnGOP::notifyWhenReadyForMoreSamples(std::function<void()>&& callback)
{
    // TODO
    UNUSED_PARAM(callback);
}

void SmoothSwitchOnGOP::enqueueSample(Ref<MediaSample>&& sample)
{
    if (m_oldSamples.empty()) { // Passthrough case
        m_innerSink->enqueueSample(WTF::move(sample));
        return;
    }
    DecodeOrderSampleMap::KeyType decodeKey { sample->decodeTime(), sample->presentationTime() };
    m_newSamples.insert({ decodeKey, WTF::move(sample) });
    // TODO: decide whether the smooth switch must be finished
}

void SmoothSwitchOnGOP::allSamplesEnqueued()
{
    // TODO: how to handle EOS? Probably just a flag, to keep things simple, as the other alternative would be a fake "EOS" MediaSample.
}

void SmoothSwitchOnGOP::flush()
{
    m_innerSink->flush();
}

void SmoothSwitchOnGOP::handleChangeInAlreadyEnqueuedContent(DecodeOrderSampleMap::MapType notYetEnqueuedSamples)
{
    for (auto& pair : notYetEnqueuedSamples) {
        DecodeOrderSampleMap::KeyType decodeKey = pair.first;
        auto it = m_oldSamples.insert_or_assign(decodeKey, protect(pair.second)).first;
        // Erase any new now following orphans
        ++it;
        while (it != m_oldSamples.end() && !protect(it->second)->isSync())
            it = m_oldSamples.erase(it);
    }
}

void SmoothSwitchOnGOP::tryFeedInnerSink()
{
    // Feed old samples
    while (!m_oldSamples.empty()) {
        if (!m_innerSink->isReadyForMoreSamples())
            return;
        Ref<MediaSample> sample = m_oldSamples.begin()->second;
        m_oldSamples.erase(m_oldSamples.begin());
        m_innerSink->enqueueSample(WTF::move(sample));
    }
    // TODO: feed new samples?
}

} // namespace WebCore