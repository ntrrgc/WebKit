/* Copyright (C) 2026 RDK Management.  All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDERS. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "config.h"

#if USE(SKIA)
#include <wtf/SystemTracing.h>

#include <skia/utils/SkEventTracer.h>
#include <skia/utils/SkTraceEventPhase.h>

namespace WebCore {

class SkWebKitTrace final : public SkEventTracer {
public:
    SkWebKitTrace() = default;

    SkEventTracer::Handle addTraceEvent(
        char phase,
        const uint8_t* categoryEnabledFlag,
        const char* name,
        uint64_t id,
        int numArgs,
        const char** argNames,
        const uint8_t* argTypes,
        const uint64_t* argValues,
        uint8_t flags) override
    {
        UNUSED_PARAM(phase);
        UNUSED_PARAM(categoryEnabledFlag);
        UNUSED_PARAM(name);
        UNUSED_PARAM(id);
        UNUSED_PARAM(numArgs);
        UNUSED_PARAM(argNames);
        UNUSED_PARAM(argTypes);
        UNUSED_PARAM(argValues);
        UNUSED_PARAM(flags);
#if USE(LINUX_FTRACE)
        if (TRACE_EVENT_PHASE_COMPLETE == phase)
            phase = TRACE_EVENT_PHASE_BEGIN;
        SystemTracingFTrace::instance().addTraceEvent(
            phase, categoryEnabledFlag, name, id,
            numArgs, argNames, argTypes, argValues, flags);
#endif
        return static_cast<SkEventTracer::Handle>(0);
    }

    void updateTraceEventDuration(const uint8_t* categoryEnabledFlag,
                                  const char* name,
                                  SkEventTracer::Handle handle) override
    {
        UNUSED_PARAM(categoryEnabledFlag);
        UNUSED_PARAM(name);
        UNUSED_PARAM(handle);
#if USE(LINUX_FTRACE)
        SystemTracingFTrace::instance().addTraceEvent(
            TRACE_EVENT_PHASE_END, categoryEnabledFlag, name, 0,
            0, nullptr, nullptr, nullptr, 0x0);
#endif
    }

    const uint8_t* getCategoryGroupEnabled(const char* name) override {
        // Skip the default prefix that Skia uses for all trace events
        const char* prefixPtr = "disabled-by-default-";
        const char* namePtr = name;
        while (*prefixPtr == *namePtr && *prefixPtr != '\0') {
            ++prefixPtr;
            ++namePtr;
        }
        return reinterpret_cast<const unsigned char*>(namePtr);
    }

    const char* getCategoryGroupName(const uint8_t* categoryEnabledFlag) override {
        UNUSED_PARAM(categoryEnabledFlag);
        static const char* category = "skiaTrace";
        return category;
    }
};

}
#endif
