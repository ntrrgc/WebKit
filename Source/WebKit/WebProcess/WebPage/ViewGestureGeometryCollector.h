/*
 * Copyright (C) 2013 Apple Inc. All rights reserved.
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

#ifndef ViewGestureGeometryCollector_h
#define ViewGestureGeometryCollector_h

#include "MessageReceiver.h"
#include <WebCore/PageIdentifier.h>
#include <wtf/RefCounted.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakRef.h>

namespace WebCore {
class FloatPoint;
class FloatRect;
class Node;
}

namespace WebKit {

class WebPage;

class ViewGestureGeometryCollector : private IPC::MessageReceiver, public RefCounted<ViewGestureGeometryCollector> {
    WTF_MAKE_TZONE_ALLOCATED(ViewGestureGeometryCollector);
public:
    static Ref<ViewGestureGeometryCollector> create(WebPage& webPage)
    {
        return adoptRef(*new ViewGestureGeometryCollector(webPage));
    }

    ~ViewGestureGeometryCollector();

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    void mainFrameDidLayout();

    void computeZoomInformationForNode(WebCore::Node&, WebCore::FloatPoint& origin, WebCore::FloatRect& absoluteBoundingRect, bool& isReplaced, double& viewportMinimumScale, double& viewportMaximumScale);

private:
    explicit ViewGestureGeometryCollector(WebPage&);

    // IPC::MessageReceiver.
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) override;

    // Message handlers.
    void collectGeometryForSmartMagnificationGesture(WebCore::FloatPoint gestureLocationInViewCoordinates);

#if !PLATFORM(IOS_FAMILY)
    void collectGeometryForMagnificationGesture();

    void setRenderTreeSizeNotificationThreshold(uint64_t);
    void sendDidHitRenderTreeSizeThresholdIfNeeded();
#endif

    void dispatchDidCollectGeometryForSmartMagnificationGesture(WebCore::FloatPoint origin, WebCore::FloatRect absoluteTargetRect, WebCore::FloatRect visibleContentRect, bool fitEntireRect, double viewportMinimumScale, double viewportMaximumScale);
    void computeMinimumAndMaximumViewportScales(double& viewportMinimumScale, double& viewportMaximumScale) const;

#if PLATFORM(IOS_FAMILY)
    std::optional<std::pair<double, double>> computeTextLegibilityScales(double& viewportMinimumScale, double& viewportMaximumScale);
#endif

    WeakPtr<WebPage> m_webPage;
    WebCore::PageIdentifier m_webPageIdentifier;

#if !PLATFORM(IOS_FAMILY)
    uint64_t m_renderTreeSizeNotificationThreshold;
#else
    std::optional<std::pair<double, double>> m_cachedTextLegibilityScales;
#endif
};

} // namespace WebKit

#endif // ViewGestureGeometryCollector
