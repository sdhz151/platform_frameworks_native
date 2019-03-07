/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
//#define LOG_NDEBUG 0

// This is needed for stdint.h to define INT64_MAX in C++
#define __STDC_LIMIT_MACROS

#include <math.h>

#include <algorithm>

#include <android-base/stringprintf.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <utils/Thread.h>
#include <utils/Trace.h>

#include <ui/FenceTime.h>

#include "DispSync.h"
#include "EventLog/EventLog.h"
#include "SurfaceFlinger.h"

using android::base::StringAppendF;
using std::max;
using std::min;

namespace android {

DispSync::~DispSync() = default;
DispSync::Callback::~Callback() = default;

namespace impl {

// Setting this to true adds a zero-phase tracer for correlating with hardware
// vsync events
static const bool kEnableZeroPhaseTracer = false;

// This is the threshold used to determine when hardware vsync events are
// needed to re-synchronize the software vsync model with the hardware.  The
// error metric used is the mean of the squared difference between each
// present time and the nearest software-predicted vsync.
static const nsecs_t kErrorThreshold = 160000000000; // 400 usec squared

#undef LOG_TAG
#define LOG_TAG "DispSyncThread"
class DispSyncThread : public Thread {
public:
    DispSyncThread(const char* name, bool showTraceDetailedInfo)
          : mName(name),
            mStop(false),
            mPeriod(0),
            mPhase(0),
            mReferenceTime(0),
            mWakeupLatency(0),
            mFrameNumber(0),
            mTraceDetailedInfo(showTraceDetailedInfo) {}

    virtual ~DispSyncThread() {}

    void updateModel(nsecs_t period, nsecs_t phase, nsecs_t referenceTime) {
        if (mTraceDetailedInfo) ATRACE_CALL();
        Mutex::Autolock lock(mMutex);
        mPeriod = period;
        mPhase = phase;
        mReferenceTime = referenceTime;
        if (mTraceDetailedInfo) {
            ATRACE_INT64("DispSync:Period", mPeriod);
            ATRACE_INT64("DispSync:Phase", mPhase + mPeriod / 2);
            ATRACE_INT64("DispSync:Reference Time", mReferenceTime);
        }
        ALOGV("[%s] updateModel: mPeriod = %" PRId64 ", mPhase = %" PRId64
              " mReferenceTime = %" PRId64,
              mName, ns2us(mPeriod), ns2us(mPhase), ns2us(mReferenceTime));
        mCond.signal();
    }

    void stop() {
        if (mTraceDetailedInfo) ATRACE_CALL();
        Mutex::Autolock lock(mMutex);
        mStop = true;
        mCond.signal();
    }

    virtual bool threadLoop() {
        status_t err;
        nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);

        while (true) {
            std::vector<CallbackInvocation> callbackInvocations;

            nsecs_t targetTime = 0;

            { // Scope for lock
                Mutex::Autolock lock(mMutex);

                if (mTraceDetailedInfo) {
                    ATRACE_INT64("DispSync:Frame", mFrameNumber);
                }
                ALOGV("[%s] Frame %" PRId64, mName, mFrameNumber);
                ++mFrameNumber;

                if (mStop) {
                    return false;
                }

                if (mPeriod == 0) {
                    err = mCond.wait(mMutex);
                    if (err != NO_ERROR) {
                        ALOGE("error waiting for new events: %s (%d)", strerror(-err), err);
                        return false;
                    }
                    continue;
                }

                targetTime = computeNextEventTimeLocked(now);

                bool isWakeup = false;

                if (now < targetTime) {
                    if (mTraceDetailedInfo) ATRACE_NAME("DispSync waiting");

                    if (targetTime == INT64_MAX) {
                        ALOGV("[%s] Waiting forever", mName);
                        err = mCond.wait(mMutex);
                    } else {
                        ALOGV("[%s] Waiting until %" PRId64, mName, ns2us(targetTime));
                        err = mCond.waitRelative(mMutex, targetTime - now);
                    }

                    if (err == TIMED_OUT) {
                        isWakeup = true;
                    } else if (err != NO_ERROR) {
                        ALOGE("error waiting for next event: %s (%d)", strerror(-err), err);
                        return false;
                    }
                }

                now = systemTime(SYSTEM_TIME_MONOTONIC);

                // Don't correct by more than 1.5 ms
                static const nsecs_t kMaxWakeupLatency = us2ns(1500);

                if (isWakeup) {
                    mWakeupLatency = ((mWakeupLatency * 63) + (now - targetTime)) / 64;
                    mWakeupLatency = min(mWakeupLatency, kMaxWakeupLatency);
                    if (mTraceDetailedInfo) {
                        ATRACE_INT64("DispSync:WakeupLat", now - targetTime);
                        ATRACE_INT64("DispSync:AvgWakeupLat", mWakeupLatency);
                    }
                }

                callbackInvocations = gatherCallbackInvocationsLocked(now);
            }

            if (callbackInvocations.size() > 0) {
                fireCallbackInvocations(callbackInvocations);
            }
        }

        return false;
    }

    status_t addEventListener(const char* name, nsecs_t phase, DispSync::Callback* callback) {
        if (mTraceDetailedInfo) ATRACE_CALL();
        Mutex::Autolock lock(mMutex);

        for (size_t i = 0; i < mEventListeners.size(); i++) {
            if (mEventListeners[i].mCallback == callback) {
                return BAD_VALUE;
            }
        }

        EventListener listener;
        listener.mName = name;
        listener.mPhase = phase;
        listener.mCallback = callback;

        // We want to allow the firstmost future event to fire without
        // allowing any past events to fire
        listener.mLastEventTime = systemTime() - mPeriod / 2 + mPhase - mWakeupLatency;

        mEventListeners.push_back(listener);

        mCond.signal();

        return NO_ERROR;
    }

    status_t removeEventListener(DispSync::Callback* callback) {
        if (mTraceDetailedInfo) ATRACE_CALL();
        Mutex::Autolock lock(mMutex);

        for (std::vector<EventListener>::iterator it = mEventListeners.begin();
             it != mEventListeners.end(); ++it) {
            if (it->mCallback == callback) {
                mEventListeners.erase(it);
                mCond.signal();
                return NO_ERROR;
            }
        }

        return BAD_VALUE;
    }

    status_t changePhaseOffset(DispSync::Callback* callback, nsecs_t phase) {
        if (mTraceDetailedInfo) ATRACE_CALL();
        Mutex::Autolock lock(mMutex);

        for (auto& eventListener : mEventListeners) {
            if (eventListener.mCallback == callback) {
                const nsecs_t oldPhase = eventListener.mPhase;
                eventListener.mPhase = phase;

                // Pretend that the last time this event was handled at the same frame but with the
                // new offset to allow for a seamless offset change without double-firing or
                // skipping.
                nsecs_t diff = oldPhase - phase;
                if (diff > mPeriod / 2) {
                    diff -= mPeriod;
                } else if (diff < -mPeriod / 2) {
                    diff += mPeriod;
                }
                eventListener.mLastEventTime -= diff;
                mCond.signal();
                return NO_ERROR;
            }
        }

        return BAD_VALUE;
    }

private:
    struct EventListener {
        const char* mName;
        nsecs_t mPhase;
        nsecs_t mLastEventTime;
        DispSync::Callback* mCallback;
    };

    struct CallbackInvocation {
        DispSync::Callback* mCallback;
        nsecs_t mEventTime;
    };

    nsecs_t computeNextEventTimeLocked(nsecs_t now) {
        if (mTraceDetailedInfo) ATRACE_CALL();
        ALOGV("[%s] computeNextEventTimeLocked", mName);
        nsecs_t nextEventTime = INT64_MAX;
        for (size_t i = 0; i < mEventListeners.size(); i++) {
            nsecs_t t = computeListenerNextEventTimeLocked(mEventListeners[i], now);

            if (t < nextEventTime) {
                nextEventTime = t;
            }
        }

        ALOGV("[%s] nextEventTime = %" PRId64, mName, ns2us(nextEventTime));
        return nextEventTime;
    }

    std::vector<CallbackInvocation> gatherCallbackInvocationsLocked(nsecs_t now) {
        if (mTraceDetailedInfo) ATRACE_CALL();
        ALOGV("[%s] gatherCallbackInvocationsLocked @ %" PRId64, mName, ns2us(now));

        std::vector<CallbackInvocation> callbackInvocations;
        nsecs_t onePeriodAgo = now - mPeriod;

        for (auto& eventListener : mEventListeners) {
            nsecs_t t = computeListenerNextEventTimeLocked(eventListener, onePeriodAgo);

            if (t < now) {
                CallbackInvocation ci;
                ci.mCallback = eventListener.mCallback;
                ci.mEventTime = t;
                ALOGV("[%s] [%s] Preparing to fire", mName, eventListener.mName);
                callbackInvocations.push_back(ci);
                eventListener.mLastEventTime = t;
            }
        }

        return callbackInvocations;
    }

    nsecs_t computeListenerNextEventTimeLocked(const EventListener& listener, nsecs_t baseTime) {
        if (mTraceDetailedInfo) ATRACE_CALL();
        ALOGV("[%s] [%s] computeListenerNextEventTimeLocked(%" PRId64 ")", mName, listener.mName,
              ns2us(baseTime));

        nsecs_t lastEventTime = listener.mLastEventTime + mWakeupLatency;
        ALOGV("[%s] lastEventTime: %" PRId64, mName, ns2us(lastEventTime));
        if (baseTime < lastEventTime) {
            baseTime = lastEventTime;
            ALOGV("[%s] Clamping baseTime to lastEventTime -> %" PRId64, mName, ns2us(baseTime));
        }

        baseTime -= mReferenceTime;
        ALOGV("[%s] Relative baseTime = %" PRId64, mName, ns2us(baseTime));
        nsecs_t phase = mPhase + listener.mPhase;
        ALOGV("[%s] Phase = %" PRId64, mName, ns2us(phase));
        baseTime -= phase;
        ALOGV("[%s] baseTime - phase = %" PRId64, mName, ns2us(baseTime));

        // If our previous time is before the reference (because the reference
        // has since been updated), the division by mPeriod will truncate
        // towards zero instead of computing the floor. Since in all cases
        // before the reference we want the next time to be effectively now, we
        // set baseTime to -mPeriod so that numPeriods will be -1.
        // When we add 1 and the phase, we will be at the correct event time for
        // this period.
        if (baseTime < 0) {
            ALOGV("[%s] Correcting negative baseTime", mName);
            baseTime = -mPeriod;
        }

        nsecs_t numPeriods = baseTime / mPeriod;
        ALOGV("[%s] numPeriods = %" PRId64, mName, numPeriods);
        nsecs_t t = (numPeriods + 1) * mPeriod + phase;
        ALOGV("[%s] t = %" PRId64, mName, ns2us(t));
        t += mReferenceTime;
        ALOGV("[%s] Absolute t = %" PRId64, mName, ns2us(t));

        // Check that it's been slightly more than half a period since the last
        // event so that we don't accidentally fall into double-rate vsyncs
        if (t - listener.mLastEventTime < (3 * mPeriod / 5)) {
            t += mPeriod;
            ALOGV("[%s] Modifying t -> %" PRId64, mName, ns2us(t));
        }

        t -= mWakeupLatency;
        ALOGV("[%s] Corrected for wakeup latency -> %" PRId64, mName, ns2us(t));

        return t;
    }

    void fireCallbackInvocations(const std::vector<CallbackInvocation>& callbacks) {
        if (mTraceDetailedInfo) ATRACE_CALL();
        for (size_t i = 0; i < callbacks.size(); i++) {
            callbacks[i].mCallback->onDispSyncEvent(callbacks[i].mEventTime);
        }
    }

    const char* const mName;

    bool mStop;

    nsecs_t mPeriod;
    nsecs_t mPhase;
    nsecs_t mReferenceTime;
    nsecs_t mWakeupLatency;

    int64_t mFrameNumber;

    std::vector<EventListener> mEventListeners;

    Mutex mMutex;
    Condition mCond;

    // Flag to turn on logging in systrace.
    const bool mTraceDetailedInfo;
};

#undef LOG_TAG
#define LOG_TAG "DispSync"

class ZeroPhaseTracer : public DispSync::Callback {
public:
    ZeroPhaseTracer() : mParity(false) {}

    virtual void onDispSyncEvent(nsecs_t /*when*/) {
        mParity = !mParity;
        ATRACE_INT("ZERO_PHASE_VSYNC", mParity ? 1 : 0);
    }

private:
    bool mParity;
};

DispSync::DispSync(const char* name) : mName(name), mRefreshSkipCount(0) {
    // This flag offers the ability to turn on systrace logging from the shell.
    char value[PROPERTY_VALUE_MAX];
    property_get("debug.sf.dispsync_trace_detailed_info", value, "0");
    mTraceDetailedInfo = atoi(value);
    mThread = new DispSyncThread(name, mTraceDetailedInfo);
}

DispSync::~DispSync() {}

void DispSync::init(bool hasSyncFramework, int64_t dispSyncPresentTimeOffset) {
    mIgnorePresentFences = !hasSyncFramework;
    mPresentTimeOffset = dispSyncPresentTimeOffset;
    mThread->run("DispSync", PRIORITY_URGENT_DISPLAY + PRIORITY_MORE_FAVORABLE);

    // set DispSync to SCHED_FIFO to minimize jitter
    struct sched_param param = {0};
    param.sched_priority = 2;
    if (sched_setscheduler(mThread->getTid(), SCHED_FIFO, &param) != 0) {
        ALOGE("Couldn't set SCHED_FIFO for DispSyncThread");
    }

    reset();
    beginResync();

    if (mTraceDetailedInfo && kEnableZeroPhaseTracer) {
        mZeroPhaseTracer = std::make_unique<ZeroPhaseTracer>();
        addEventListener("ZeroPhaseTracer", 0, mZeroPhaseTracer.get());
    }
}

void DispSync::reset() {
    Mutex::Autolock lock(mMutex);
    resetLocked();
}

void DispSync::resetLocked() {
    mPhase = 0;
    mReferenceTime = 0;
    mModelUpdated = false;
    mNumResyncSamples = 0;
    mFirstResyncSample = 0;
    mNumResyncSamplesSincePresent = 0;
    resetErrorLocked();
}

bool DispSync::addPresentFence(const std::shared_ptr<FenceTime>& fenceTime) {
    Mutex::Autolock lock(mMutex);

    if (mIgnorePresentFences) {
        return true;
    }

    mPresentFences[mPresentSampleOffset] = fenceTime;
    mPresentSampleOffset = (mPresentSampleOffset + 1) % NUM_PRESENT_SAMPLES;
    mNumResyncSamplesSincePresent = 0;

    updateErrorLocked();

    return !mModelUpdated || mError > kErrorThreshold;
}

void DispSync::beginResync() {
    Mutex::Autolock lock(mMutex);
    ALOGV("[%s] beginResync", mName);
    mModelUpdated = false;
    mNumResyncSamples = 0;
}

bool DispSync::addResyncSample(nsecs_t timestamp) {
    Mutex::Autolock lock(mMutex);

    ALOGV("[%s] addResyncSample(%" PRId64 ")", mName, ns2us(timestamp));

    size_t idx = (mFirstResyncSample + mNumResyncSamples) % MAX_RESYNC_SAMPLES;
    mResyncSamples[idx] = timestamp;
    if (mNumResyncSamples == 0) {
        mPhase = 0;
        mReferenceTime = timestamp;
        ALOGV("[%s] First resync sample: mPeriod = %" PRId64 ", mPhase = 0, "
              "mReferenceTime = %" PRId64,
              mName, ns2us(mPeriod), ns2us(mReferenceTime));
        mThread->updateModel(mPeriod, mPhase, mReferenceTime);
    }

    if (mNumResyncSamples < MAX_RESYNC_SAMPLES) {
        mNumResyncSamples++;
    } else {
        mFirstResyncSample = (mFirstResyncSample + 1) % MAX_RESYNC_SAMPLES;
    }

    updateModelLocked();

    if (mNumResyncSamplesSincePresent++ > MAX_RESYNC_SAMPLES_WITHOUT_PRESENT) {
        resetErrorLocked();
    }

    if (mIgnorePresentFences) {
        // If we're ignoring the present fences we have no way to know whether
        // or not we're synchronized with the HW vsyncs, so we just request
        // that the HW vsync events be turned on.
        return true;
    }

    // Check against kErrorThreshold / 2 to add some hysteresis before having to
    // resync again
    bool modelLocked = mModelUpdated && mError < (kErrorThreshold / 2);
    ALOGV("[%s] addResyncSample returning %s", mName, modelLocked ? "locked" : "unlocked");
    return !modelLocked;
}

void DispSync::endResync() {}

status_t DispSync::addEventListener(const char* name, nsecs_t phase, Callback* callback) {
    Mutex::Autolock lock(mMutex);
    return mThread->addEventListener(name, phase, callback);
}

void DispSync::setRefreshSkipCount(int count) {
    Mutex::Autolock lock(mMutex);
    ALOGD("setRefreshSkipCount(%d)", count);
    mRefreshSkipCount = count;
    updateModelLocked();
}

status_t DispSync::removeEventListener(Callback* callback) {
    Mutex::Autolock lock(mMutex);
    return mThread->removeEventListener(callback);
}

status_t DispSync::changePhaseOffset(Callback* callback, nsecs_t phase) {
    Mutex::Autolock lock(mMutex);
    return mThread->changePhaseOffset(callback, phase);
}

void DispSync::setPeriod(nsecs_t period) {
    Mutex::Autolock lock(mMutex);
    mPeriod = period;
    mPhase = 0;
    mReferenceTime = 0;
    mThread->updateModel(mPeriod, mPhase, mReferenceTime);
}

nsecs_t DispSync::getPeriod() {
    // lock mutex as mPeriod changes multiple times in updateModelLocked
    Mutex::Autolock lock(mMutex);
    return mPeriod;
}

void DispSync::updateModelLocked() {
    ALOGV("[%s] updateModelLocked %zu", mName, mNumResyncSamples);
    if (mNumResyncSamples >= MIN_RESYNC_SAMPLES_FOR_UPDATE) {
        ALOGV("[%s] Computing...", mName);
        nsecs_t durationSum = 0;
        nsecs_t minDuration = INT64_MAX;
        nsecs_t maxDuration = 0;
        for (size_t i = 1; i < mNumResyncSamples; i++) {
            size_t idx = (mFirstResyncSample + i) % MAX_RESYNC_SAMPLES;
            size_t prev = (idx + MAX_RESYNC_SAMPLES - 1) % MAX_RESYNC_SAMPLES;
            nsecs_t duration = mResyncSamples[idx] - mResyncSamples[prev];
            durationSum += duration;
            minDuration = min(minDuration, duration);
            maxDuration = max(maxDuration, duration);
        }

        // Exclude the min and max from the average
        durationSum -= minDuration + maxDuration;
        mPeriod = durationSum / (mNumResyncSamples - 3);

        ALOGV("[%s] mPeriod = %" PRId64, mName, ns2us(mPeriod));

        double sampleAvgX = 0;
        double sampleAvgY = 0;
        double scale = 2.0 * M_PI / double(mPeriod);
        // Intentionally skip the first sample
        for (size_t i = 1; i < mNumResyncSamples; i++) {
            size_t idx = (mFirstResyncSample + i) % MAX_RESYNC_SAMPLES;
            nsecs_t sample = mResyncSamples[idx] - mReferenceTime;
            double samplePhase = double(sample % mPeriod) * scale;
            sampleAvgX += cos(samplePhase);
            sampleAvgY += sin(samplePhase);
        }

        sampleAvgX /= double(mNumResyncSamples - 1);
        sampleAvgY /= double(mNumResyncSamples - 1);

        mPhase = nsecs_t(atan2(sampleAvgY, sampleAvgX) / scale);

        ALOGV("[%s] mPhase = %" PRId64, mName, ns2us(mPhase));

        if (mPhase < -(mPeriod / 2)) {
            mPhase += mPeriod;
            ALOGV("[%s] Adjusting mPhase -> %" PRId64, mName, ns2us(mPhase));
        }

        // Artificially inflate the period if requested.
        mPeriod += mPeriod * mRefreshSkipCount;

        mThread->updateModel(mPeriod, mPhase, mReferenceTime);
        mModelUpdated = true;
    }
}

void DispSync::updateErrorLocked() {
    if (!mModelUpdated) {
        return;
    }

    // Need to compare present fences against the un-adjusted refresh period,
    // since they might arrive between two events.
    nsecs_t period = mPeriod / (1 + mRefreshSkipCount);

    int numErrSamples = 0;
    nsecs_t sqErrSum = 0;

    for (size_t i = 0; i < NUM_PRESENT_SAMPLES; i++) {
        // Only check for the cached value of signal time to avoid unecessary
        // syscalls. It is the responsibility of the DispSync owner to
        // call getSignalTime() periodically so the cache is updated when the
        // fence signals.
        nsecs_t time = mPresentFences[i]->getCachedSignalTime();
        if (time == Fence::SIGNAL_TIME_PENDING || time == Fence::SIGNAL_TIME_INVALID) {
            continue;
        }

        nsecs_t sample = time - mReferenceTime;
        if (sample <= mPhase) {
            continue;
        }

        nsecs_t sampleErr = (sample - mPhase) % period;
        if (sampleErr > period / 2) {
            sampleErr -= period;
        }
        sqErrSum += sampleErr * sampleErr;
        numErrSamples++;
    }

    if (numErrSamples > 0) {
        mError = sqErrSum / numErrSamples;
        mZeroErrSamplesCount = 0;
    } else {
        mError = 0;
        // Use mod ACCEPTABLE_ZERO_ERR_SAMPLES_COUNT to avoid log spam.
        mZeroErrSamplesCount++;
        ALOGE_IF((mZeroErrSamplesCount % ACCEPTABLE_ZERO_ERR_SAMPLES_COUNT) == 0,
                 "No present times for model error.");
    }

    if (mTraceDetailedInfo) {
        ATRACE_INT64("DispSync:Error", mError);
    }
}

void DispSync::resetErrorLocked() {
    mPresentSampleOffset = 0;
    mError = 0;
    mZeroErrSamplesCount = 0;
    for (size_t i = 0; i < NUM_PRESENT_SAMPLES; i++) {
        mPresentFences[i] = FenceTime::NO_FENCE;
    }
}

nsecs_t DispSync::computeNextRefresh(int periodOffset) const {
    Mutex::Autolock lock(mMutex);
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    nsecs_t phase = mReferenceTime + mPhase;
    if (mPeriod == 0) {
        return 0;
    }
    return (((now - phase) / mPeriod) + periodOffset + 1) * mPeriod + phase;
}

void DispSync::setIgnorePresentFences(bool ignore) {
    Mutex::Autolock lock(mMutex);
    if (mIgnorePresentFences != ignore) {
        mIgnorePresentFences = ignore;
        resetLocked();
    }
}

void DispSync::dump(std::string& result) const {
    Mutex::Autolock lock(mMutex);
    StringAppendF(&result, "present fences are %s\n", mIgnorePresentFences ? "ignored" : "used");
    StringAppendF(&result, "mPeriod: %" PRId64 " ns (%.3f fps; skipCount=%d)\n", mPeriod,
                  1000000000.0 / mPeriod, mRefreshSkipCount);
    StringAppendF(&result, "mPhase: %" PRId64 " ns\n", mPhase);
    StringAppendF(&result, "mError: %" PRId64 " ns (sqrt=%.1f)\n", mError, sqrt(mError));
    StringAppendF(&result, "mNumResyncSamplesSincePresent: %d (limit %d)\n",
                  mNumResyncSamplesSincePresent, MAX_RESYNC_SAMPLES_WITHOUT_PRESENT);
    StringAppendF(&result, "mNumResyncSamples: %zd (max %d)\n", mNumResyncSamples,
                  MAX_RESYNC_SAMPLES);

    result.append("mResyncSamples:\n");
    nsecs_t previous = -1;
    for (size_t i = 0; i < mNumResyncSamples; i++) {
        size_t idx = (mFirstResyncSample + i) % MAX_RESYNC_SAMPLES;
        nsecs_t sampleTime = mResyncSamples[idx];
        if (i == 0) {
            StringAppendF(&result, "  %" PRId64 "\n", sampleTime);
        } else {
            StringAppendF(&result, "  %" PRId64 " (+%" PRId64 ")\n", sampleTime,
                          sampleTime - previous);
        }
        previous = sampleTime;
    }

    StringAppendF(&result, "mPresentFences [%d]:\n", NUM_PRESENT_SAMPLES);
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    previous = Fence::SIGNAL_TIME_INVALID;
    for (size_t i = 0; i < NUM_PRESENT_SAMPLES; i++) {
        size_t idx = (i + mPresentSampleOffset) % NUM_PRESENT_SAMPLES;
        nsecs_t presentTime = mPresentFences[idx]->getSignalTime();
        if (presentTime == Fence::SIGNAL_TIME_PENDING) {
            StringAppendF(&result, "  [unsignaled fence]\n");
        } else if (presentTime == Fence::SIGNAL_TIME_INVALID) {
            StringAppendF(&result, "  [invalid fence]\n");
        } else if (previous == Fence::SIGNAL_TIME_PENDING ||
                   previous == Fence::SIGNAL_TIME_INVALID) {
            StringAppendF(&result, "  %" PRId64 "  (%.3f ms ago)\n", presentTime,
                          (now - presentTime) / 1000000.0);
        } else {
            StringAppendF(&result, "  %" PRId64 " (+%" PRId64 " / %.3f)  (%.3f ms ago)\n",
                          presentTime, presentTime - previous,
                          (presentTime - previous) / (double)mPeriod,
                          (now - presentTime) / 1000000.0);
        }
        previous = presentTime;
    }

    StringAppendF(&result, "current monotonic time: %" PRId64 "\n", now);
}

// TODO(b/113612090): Figure out how much of this is still relevant.
// We need to determine the time when a buffer acquired now will be
// displayed.  This can be calculated:
//   time when previous buffer's actual-present fence was signaled
//    + current display refresh rate * HWC latency
//    + a little extra padding
//
// Buffer producers are expected to set their desired presentation time
// based on choreographer time stamps, which (coming from vsync events)
// will be slightly later then the actual-present timing.  If we get a
// desired-present time that is unintentionally a hair after the next
// vsync, we'll hold the frame when we really want to display it.  We
// need to take the offset between actual-present and reported-vsync
// into account.
//
// If the system is configured without a DispSync phase offset for the app,
// we also want to throw in a bit of padding to avoid edge cases where we
// just barely miss.  We want to do it here, not in every app.  A major
// source of trouble is the app's use of the display's ideal refresh time
// (via Display.getRefreshRate()), which could be off of the actual refresh
// by a few percent, with the error multiplied by the number of frames
// between now and when the buffer should be displayed.
//
// If the refresh reported to the app has a phase offset, we shouldn't need
// to tweak anything here.
nsecs_t DispSync::expectedPresentTime() {
    // The HWC doesn't currently have a way to report additional latency.
    // Assume that whatever we submit now will appear right after the flip.
    // For a smart panel this might be 1.  This is expressed in frames,
    // rather than time, because we expect to have a constant frame delay
    // regardless of the refresh rate.
    const uint32_t hwcLatency = 0;

    // Ask DispSync when the next refresh will be (CLOCK_MONOTONIC).
    return computeNextRefresh(hwcLatency);
}

} // namespace impl

} // namespace android
