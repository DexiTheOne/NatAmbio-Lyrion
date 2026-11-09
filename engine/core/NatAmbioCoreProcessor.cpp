#include "NatAmbioCoreProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace {
constexpr int kNaeWindowCrossfadeSamples = 2048;
constexpr int kXtcWarmWorkMultiplier = 4;
constexpr std::uintptr_t kEmptyXtcCommand = 0;
constexpr std::uintptr_t kDisableXtcCommand = 1;

static_assert(alignof(XtcProcessor) >= 2,
              "XTC mailbox reserves the low pointer bit for commands");
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free,
              "XTC publication must remain lock-free on the audio thread");
static_assert(std::atomic<XtcProcessor*>::is_always_lock_free,
              "XTC retirement must remain lock-free on the audio thread");

bool isXtcProcessorCommand(std::uintptr_t command) noexcept
{
    return command != kEmptyXtcCommand
        && (command & kDisableXtcCommand) == 0;
}

std::uintptr_t makeXtcProcessorCommand(std::unique_ptr<XtcProcessor> processor) noexcept
{
    if (!processor)
        return kDisableXtcCommand;

    const auto command = reinterpret_cast<std::uintptr_t>(processor.release());
    return command;
}

XtcProcessor* xtcProcessorFromCommand(std::uintptr_t command) noexcept
{
    return reinterpret_cast<XtcProcessor*>(command);
}

void destroyXtcProcessorCommand(std::uintptr_t command) noexcept
{
    if (isXtcProcessorCommand(command))
        delete xtcProcessorFromCommand(command);
}

}

NatAmbioCoreProcessor::NatAmbioCoreProcessor(int sr, int bs)
    : sampleRate(sr), blockSize(bs)
{
    prepare(sr, bs);
}

NatAmbioCoreProcessor::~NatAmbioCoreProcessor()
{
    // Host lifecycle guarantees that processing has stopped before teardown.
    // Raw mailbox ownership cannot be left for atomic destructors to leak.
    clearXtcOwnershipOnStoppedThread();
}

void NatAmbioCoreProcessor::prepare(int sr, int bs)
{
    sampleRate = sr;
    blockSize = std::max(1, bs);

    if (!nae_processor) {
        nae_processor = std::make_unique<NaeProcessor>(sampleRate, blockSize);
    }
    nae_processor->prepare(sampleRate, blockSize);
    if (!fading_nae_processor) {
        fading_nae_processor = std::make_unique<NaeProcessor>(sampleRate, blockSize);
    }
    fading_nae_processor->prepare(sampleRate, blockSize);

    // prepare() is a stopped-audio lifecycle operation. Reclaim every owner
    // before rebuilding storage for the new sample rate and block size.
    clearXtcOwnershipOnStoppedThread();
    active_xtc_owner = std::make_unique<XtcProcessor>(sampleRate, blockSize);
    active_xtc_owner->prepare(sampleRate, blockSize);
    warming_xtc_owner.reset();
    fading_xtc_owner.reset();

    sanitized_input_left.assign(static_cast<size_t>(blockSize), 0.0f);
    sanitized_input_right.assign(static_cast<size_t>(blockSize), 0.0f);
    nae_left_out.assign(static_cast<size_t>(blockSize), 0.0f);
    nae_right_out.assign(static_cast<size_t>(blockSize), 0.0f);
    naeDryHistoryLeft.assign(static_cast<size_t>(ICORRL) * blockSize, 0.0f);
    naeDryHistoryRight.assign(naeDryHistoryLeft.size(), 0.0f);
    bypassFadeStep = 1.0f / std::max(1.0f, 0.015f * static_cast<float>(sampleRate));
    bypassMixInitialized = false;
    buffered_input_left.assign(static_cast<size_t>(blockSize), 0.0f);
    buffered_input_right.assign(static_cast<size_t>(blockSize), 0.0f);
    buffered_output_left.assign(static_cast<size_t>(blockSize), 0.0f);
    buffered_output_right.assign(static_cast<size_t>(blockSize), 0.0f);
    buffered_nae_tap_left.assign(static_cast<size_t>(blockSize), 0.0f);
    buffered_nae_tap_right.assign(static_cast<size_t>(blockSize), 0.0f);
    output_fifo_left.assign(static_cast<size_t>(blockSize * 4), 0.0f);
    output_fifo_right.assign(static_cast<size_t>(blockSize * 4), 0.0f);
    nae_tap_fifo_left.assign(static_cast<size_t>(blockSize * 4), 0.0f);
    nae_tap_fifo_right.assign(static_cast<size_t>(blockSize * 4), 0.0f);
    naeFadeOldLeft.assign(static_cast<size_t>(blockSize), 0.0f);
    naeFadeOldRight.assign(static_cast<size_t>(blockSize), 0.0f);
    naeGainMainRamp.assign(static_cast<size_t>(blockSize), 0.0);
    naeGainAmbRamp.assign(static_cast<size_t>(blockSize), 0.0);

    xtcFadeOldLeft.assign(static_cast<size_t>(blockSize), 0.0f);
    xtcFadeOldRight.assign(static_cast<size_t>(blockSize), 0.0f);

    // Keep enough post-NAE input to pre-roll any supported replacement FIR.
    // A separate bounded backlog lets the audio thread warm a replacement
    // incrementally instead of replaying the complete FIR history in one call.
    const int xtcHistoryBlocks = std::max(1, (XTC_FILTER_LEN_MAX + blockSize - 1) / blockSize);
    const int xtcHistoryCapacity = xtcHistoryBlocks * blockSize;
    const int xtcWarmBacklogCapacity = xtcHistoryCapacity + 2 * blockSize;
    xtcHistoryLeft.assign(static_cast<size_t>(xtcHistoryCapacity), 0.0f);
    xtcHistoryRight.assign(static_cast<size_t>(xtcHistoryCapacity), 0.0f);
    xtcWarmInputLeft.assign(static_cast<size_t>(blockSize), 0.0f);
    xtcWarmInputRight.assign(static_cast<size_t>(blockSize), 0.0f);
    xtcWarmOutputLeft.assign(static_cast<size_t>(blockSize), 0.0f);
    xtcWarmOutputRight.assign(static_cast<size_t>(blockSize), 0.0f);
    xtcWarmBacklogLeft.assign(static_cast<size_t>(xtcWarmBacklogCapacity), 0.0f);
    xtcWarmBacklogRight.assign(static_cast<size_t>(xtcWarmBacklogCapacity), 0.0f);
    xtcHistoryWritePosition = 0;
    xtcHistorySamples = 0;
    xtcHistoryStreamPhase = 0;
    xtcWarmBacklogReadPosition = 0;
    xtcWarmBacklogWritePosition = 0;
    xtcWarmBacklogSamples = 0;
    lastXtcProcessingEnabled = false;

    // Per-sample retention. Block processing raises this value to the exact
    // number of elapsed samples instead of applying a per-sample coefficient
    // just once per callback.
    {
        const double timeMs = NATAMBIO_GAIN_SMOOTH_TIME_MS;
        const double tau = (timeMs / 1000.0) * std::max(1, sampleRate);
        gainSmoothRetentionPerSample = tau > 0.0 ? std::exp(-1.0 / tau) : 0.0;
    }

    // Initialize smoothed values to sensible defaults
    smoothedNaeGainMain = 0.0;
    smoothedNaeGainAmb = 0.0;
    smoothedXtcAmount = 1.0f;
    targetNaeGainMain = 0.0;
    targetNaeGainAmb = 0.0;
    targetXtcAmount = 1.0f;

    xtcCrossfadeRemaining = 0;
    xtcCrossfadeLength = std::max(128, NATAMBIO_XTC_CROSSFADE_SAMPLES);
    fadingNaeActive = false;
    naeCrossfadeRemaining = 0;
    naeCrossfadeLength = std::max(blockSize, kNaeWindowCrossfadeSamples);
    lastEffectiveNaeConfig = NaeStereoConfig {};
    fadingNaeConfig = NaeStereoConfig {};
    lastEffectiveNaeConfigValid = false;

    resetBlockAdapter();
    fixedFrameAdapterActive = false;
}

void NatAmbioCoreProcessor::configureXtc(const XtcStereoConfig& config)
{
    // Setup-only API: realtime rebuilds publish a separately prepared owner via
    // setXtcProcessor().
    if (active_xtc_owner) {
        active_xtc_owner->configure(config);
    }
}

void NatAmbioCoreProcessor::setXtcProcessor(std::unique_ptr<XtcProcessor> processor)
{
    // Reclamation and coalescing happen only on non-audio callers. exchange()
    // linearizes overlapping publishers and leaves the audio thread with one
    // indivisible command to claim.
    reclaimRetiredXtcOwner();
    const auto command = makeXtcProcessorCommand(std::move(processor));
    const auto superseded = pending_xtc_command.exchange(command,
                                                          std::memory_order_acq_rel);
    destroyXtcProcessorCommand(superseded);
}

NaeProcessor::PcaAnalysis NatAmbioCoreProcessor::getNaePcaAnalysis() const noexcept
{
    return nae_processor != nullptr ? nae_processor->getPcaAnalysis()
                                    : NaeProcessor::PcaAnalysis {};
}

void NatAmbioCoreProcessor::setPcaReporting(bool enabled) noexcept
{
    if (nae_processor)
        nae_processor->setPcaReporting(enabled);
    if (fading_nae_processor)
        fading_nae_processor->setPcaReporting(enabled);
}

void NatAmbioCoreProcessor::process(const float* left_in, const float* right_in,
                                    float* left_out, float* right_out,
                                    int numSamples,
                                    const NatAmbioStereoConfig& config)
{
    processWithNaeTap(left_in, right_in, left_out, right_out, nullptr, nullptr, numSamples, config);
}

void NatAmbioCoreProcessor::processWithNaeTap(const float* left_in, const float* right_in,
                                              float* left_out, float* right_out,
                                              float* nae_tap_left, float* nae_tap_right,
                                              int numSamples,
                                              const NatAmbioStereoConfig& config)
{
    if (!nae_processor || !left_in || !right_in || !left_out || !right_out)
        return;
    if (numSamples <= 0)
        return;

    if (!bypassMixInitialized) {
        naeBypassMix = config.nae_bypassed ? 1.0f : 0.0f;
        xtcBypassMix = config.xtc_bypassed ? 1.0f : 0.0f;
        bypassMixInitialized = true;
    }

    // Update targets for smoothing (called every process; smoothing happens inside fixed block)
    const double primaryGain = config.nae.mode == NaeMode::Rear
        ? config.nae.gain_surr : config.nae.gain_main;
    targetNaeGainMain = std::isfinite(primaryGain) ? primaryGain : 0.0;
    targetNaeGainAmb = std::isfinite(config.nae.gain_amb) ? config.nae.gain_amb : 0.0;
    targetXtcAmount = std::isfinite(config.xtc.amount)
        ? std::clamp(config.xtc.amount, 0.0f, 1.0f)
        : 0.0f;

    // NAE's covariance buckets require fixed internal frames. Output from a
    // frame becomes visible on the following sample, giving one deterministic
    // block of adapter latency for fixed and irregular host callback patterns.
    // XTC alone supports arbitrary callback sizes and remains zero-latency.
    const bool requiresFixedFrames = config.nae_enabled;
    if (requiresFixedFrames != fixedFrameAdapterActive) {
        resetBlockAdapter();
        fixedFrameAdapterActive = requiresFixedFrames;
    }
    if (!requiresFixedFrames) {
        int offset = 0;
        while (offset < numSamples) {
            const int samplesThisBlock = std::min(blockSize, numSamples - offset);
            processFixedBlock(left_in + offset, right_in + offset,
                              left_out + offset, right_out + offset,
                              nae_tap_left != nullptr ? nae_tap_left + offset : nullptr,
                              nae_tap_right != nullptr ? nae_tap_right + offset : nullptr,
                              config, samplesThisBlock);
            offset += samplesThisBlock;
        }
#if NATAMBIO_VERIFICATION_BUILD
        if (capture_target_)
            StageCapture::appendBlock(left_out, right_out, numSamples,
                                      capture_target_->final_L, capture_target_->final_R);
#endif
        return;
    }

    // Each fixed-frame input slice is copied before its output slice is
    // written, so ordinary in-place stereo needs no callback-sized staging.
    processBufferedBlocks(left_in, right_in, left_out, right_out,
                          nae_tap_left, nae_tap_right,
                          numSamples, config);

#if NATAMBIO_VERIFICATION_BUILD
    if (capture_target_)
        StageCapture::appendBlock(left_out, right_out, numSamples,
                                  capture_target_->final_L, capture_target_->final_R);
#endif
}

void NatAmbioCoreProcessor::processBufferedBlocks(const float* left_in, const float* right_in,
                                                  float* left_out, float* right_out,
                                                  float* nae_tap_left, float* nae_tap_right,
                                                  int numSamples,
                                                  const NatAmbioStereoConfig& config)
{
    int offset = 0;
    while (offset < numSamples) {
        const int frameSpace = blockSize - inputFrameFill;
        const int samplesToCopy = std::min(frameSpace, numSamples - offset);

        std::memcpy(buffered_input_left.data() + inputFrameFill,
                    left_in + offset,
                    static_cast<size_t>(samplesToCopy) * sizeof(float));
        std::memcpy(buffered_input_right.data() + inputFrameFill,
                    right_in + offset,
                    static_cast<size_t>(samplesToCopy) * sizeof(float));

        const int popped = popOutputSamples(
            left_out + offset,
            right_out + offset,
            nae_tap_left != nullptr ? nae_tap_left + offset : nullptr,
            nae_tap_right != nullptr ? nae_tap_right + offset : nullptr,
            samplesToCopy);
        if (popped < samplesToCopy) {
            const auto bytes = static_cast<size_t>(samplesToCopy - popped) * sizeof(float);
            std::memset(left_out + offset + popped, 0, bytes);
            std::memset(right_out + offset + popped, 0, bytes);
            if (nae_tap_left != nullptr)
                std::memset(nae_tap_left + offset + popped, 0, bytes);
            if (nae_tap_right != nullptr)
                std::memset(nae_tap_right + offset + popped, 0, bytes);
        }

        inputFrameFill += samplesToCopy;
        offset += samplesToCopy;

        if (inputFrameFill == blockSize) {
            processFixedBlock(buffered_input_left.data(), buffered_input_right.data(),
                              buffered_output_left.data(), buffered_output_right.data(),
                              buffered_nae_tap_left.data(), buffered_nae_tap_right.data(),
                              config, blockSize);
            pushOutputSamples(buffered_output_left.data(), buffered_output_right.data(),
                              buffered_nae_tap_left.data(), buffered_nae_tap_right.data(),
                              blockSize);
            inputFrameFill = 0;
        }
    }
}

void NatAmbioCoreProcessor::processFixedBlock(const float* left_in, const float* right_in,
                                              float* left_out, float* right_out,
                                              float* nae_tap_left, float* nae_tap_right,
                                              const NatAmbioStereoConfig& config,
                                              int numSamples)
{
    numSamples = std::max(0, std::min(numSamples, blockSize));
    if (numSamples == 0)
        return;

    sanitizeInputBlock(left_in, right_in, numSamples);

    // A first static configuration is state, not automation: start at its
    // supplied gains. Later changes retain the 10 ms per-sample smoothing.
    if (!lastEffectiveNaeConfigValid) {
        smoothedNaeGainMain = targetNaeGainMain;
        smoothedNaeGainAmb = targetNaeGainAmb;
    }

    // Build true per-sample dB ramps so automation keeps its 10 ms time
    // constant without staircase jumps at internal-frame boundaries.
    fillNaeGainRamps(numSamples);
    NaeStereoConfig  naeWorking = config.nae;
    XtcStereoConfig  xtcWorking = config.xtc;
    naeWorking.gain_main = smoothedNaeGainMain;
    naeWorking.gain_amb = smoothedNaeGainAmb;
    // Process full-wet so the FIR history remains warm even when amount is zero;
    // the smoothed wet/dry amount is applied per sample below.
    xtcWorking.amount = 1.0f;

    const float* stage_left = sanitized_input_left.data();
    const float* stage_right = sanitized_input_right.data();

#if NATAMBIO_VERIFICATION_BUILD
    if (capture_target_) {
        StageCapture::appendBlock(stage_left, stage_right, numSamples,
                                  capture_target_->input_L, capture_target_->input_R);
        capture_target_->nae_was_active = config.nae_enabled;
        capture_target_->xtc_was_active = config.xtc_enabled;
    }
#endif

    if (config.nae_enabled) {
        const int newSteps = clampNaeSteps(naeWorking.steps_length);
        const int previousSteps = lastEffectiveNaeConfigValid
            ? clampNaeSteps(lastEffectiveNaeConfig.steps_length)
            : newSteps;
        if (lastEffectiveNaeConfigValid
            && newSteps != previousSteps
            && fading_nae_processor) {
            fading_nae_processor->copyStateFrom(*nae_processor);
            fadingNaeConfig = lastEffectiveNaeConfig;
            fadingNaeActive = true;
            naeCrossfadeRemaining = naeCrossfadeLength;
        }

        nae_processor->processWithGainRamps(stage_left, stage_right,
                                            nae_left_out.data(), nae_right_out.data(),
                                            numSamples, naeWorking,
                                            naeGainMainRamp.data(),
                                            naeGainAmbRamp.data());

        if (fadingNaeActive && naeCrossfadeRemaining > 0 && fading_nae_processor) {
            fading_nae_processor->processWithGainRamps(stage_left, stage_right,
                                                       naeFadeOldLeft.data(), naeFadeOldRight.data(),
                                                       numSamples, fadingNaeConfig,
                                                       naeGainMainRamp.data(),
                                                       naeGainAmbRamp.data());

            const int fadeLen = std::max(1, naeCrossfadeLength);
            const int fadeStart = fadeLen - naeCrossfadeRemaining;
            for (int i = 0; i < numSamples; ++i) {
                const float r = std::clamp(static_cast<float>(fadeStart + i + 1)
                                               / static_cast<float>(fadeLen),
                                           0.0f,
                                           1.0f);
                nae_left_out[static_cast<size_t>(i)] =
                    naeFadeOldLeft[static_cast<size_t>(i)] * (1.0f - r)
                    + nae_left_out[static_cast<size_t>(i)] * r;
                nae_right_out[static_cast<size_t>(i)] =
                    naeFadeOldRight[static_cast<size_t>(i)] * (1.0f - r)
                    + nae_right_out[static_cast<size_t>(i)] * r;
            }

            naeCrossfadeRemaining = std::max(0, naeCrossfadeRemaining - numSamples);
            if (naeCrossfadeRemaining <= 0)
                fadingNaeActive = false;
        }

        blendNaeBypass(config, numSamples);
        stage_left = nae_left_out.data();
        stage_right = nae_right_out.data();
        lastEffectiveNaeConfig = naeWorking;
        lastEffectiveNaeConfigValid = true;

#if NATAMBIO_VERIFICATION_BUILD
        if (capture_target_) {
            StageCapture::appendBlock(stage_left, stage_right, numSamples,
                                      capture_target_->nae_out_L, capture_target_->nae_out_R);
        }
#endif
    } else {
        lastEffectiveNaeConfigValid = false;
        fadingNaeActive = false;
        naeCrossfadeRemaining = 0;
    }

    if (nae_tap_left != nullptr && nae_tap_right != nullptr) {
        std::memcpy(nae_tap_left, stage_left, static_cast<size_t>(numSamples) * sizeof(float));
        std::memcpy(nae_tap_right, stage_right, static_cast<size_t>(numSamples) * sizeof(float));
    }

#if NATAMBIO_VERIFICATION_BUILD
    if (capture_target_) {
        StageCapture::appendBlock(stage_left, stage_right, numSamples,
                                  capture_target_->xtc_in_L, capture_target_->xtc_in_R);
    }
#endif

    if (config.xtc_enabled) {
        auto* currentActive = active_xtc_owner.get();

        // If XTC was disabled, rebuild the active processor's state from the
        // same bounded history used by the incremental hot-swap warmer.
        if (!lastXtcProcessingEnabled && currentActive) {
            currentActive->clearDelayBuffers();
            warmXtcProcessor(*currentActive, xtcWorking);
        }

        if (fading_xtc_owner && xtcCrossfadeRemaining > 0) {
            fading_xtc_owner->process(stage_left, stage_right,
                                      xtcFadeOldLeft.data(), xtcFadeOldRight.data(),
                                      numSamples, xtcWorking);

            if (currentActive) {
                currentActive->process(stage_left, stage_right,
                                       left_out, right_out,
                                       numSamples, xtcWorking);
            } else {
                std::memcpy(left_out, stage_left, static_cast<size_t>(numSamples) * sizeof(float));
                std::memcpy(right_out, stage_right, static_cast<size_t>(numSamples) * sizeof(float));
            }

            const int fadeLen = std::max(1, xtcCrossfadeLength);
            for (int i = 0; i < numSamples; ++i) {
                const float remaining = static_cast<float>(xtcCrossfadeRemaining - i - 1);
                const float r = 1.0f - std::clamp(remaining / static_cast<float>(fadeLen),
                                                  0.0f,
                                                  1.0f);
                left_out[i] = xtcFadeOldLeft[static_cast<size_t>(i)] * (1.0f - r)
                    + left_out[i] * r;
                right_out[i] = xtcFadeOldRight[static_cast<size_t>(i)] * (1.0f - r)
                    + right_out[i] * r;
            }

            xtcCrossfadeRemaining = std::max(0, xtcCrossfadeRemaining - numSamples);
            retireFinishedXtcOwner();
        } else if (currentActive) {
            currentActive->process(stage_left, stage_right,
                                   left_out, right_out,
                                   numSamples, xtcWorking);
        } else {
            std::memcpy(left_out, stage_left, static_cast<size_t>(numSamples) * sizeof(float));
            std::memcpy(right_out, stage_right, static_cast<size_t>(numSamples) * sizeof(float));
        }

        for (int i = 0; i < numSamples; ++i) {
            xtcBypassMix = advanceBypassMix(xtcBypassMix, config.xtc_bypassed);
            const float amount = nextSmoothedXtcAmount() * (1.0f - xtcBypassMix);
            left_out[i] = amount == 0.0f ? stage_left[i]
                : stage_left[i] + amount * (left_out[i] - stage_left[i]);
            right_out[i] = amount == 0.0f ? stage_right[i]
                : stage_right[i] + amount * (right_out[i] - stage_right[i]);
        }

#if NATAMBIO_VERIFICATION_BUILD
        if (capture_target_) {
            StageCapture::appendBlock(left_out, right_out, numSamples,
                                      capture_target_->xtc_out_L, capture_target_->xtc_out_R);
        }
#endif
        lastXtcProcessingEnabled = true;
        beginPendingXtcWarm();
    } else {
        std::memcpy(left_out, stage_left, static_cast<size_t>(numSamples) * sizeof(float));
        std::memcpy(right_out, stage_right, static_cast<size_t>(numSamples) * sizeof(float));
        for (int i = 0; i < numSamples; ++i)
            (void) nextSmoothedXtcAmount();
        lastXtcProcessingEnabled = false;
    }

    if (warming_xtc_owner) {
        appendWarmingXtcInput(stage_left, stage_right, numSamples);
        advanceWarmingXtc(xtcWorking, numSamples, config.xtc_enabled);
    }

    appendXtcHistory(stage_left, stage_right, numSamples);
    sanitizeOutputBlock(left_out, right_out, numSamples);
}

float NatAmbioCoreProcessor::advanceBypassMix(float mix, bool bypassed) const noexcept
{
    return bypassed ? std::min(1.0f, mix + bypassFadeStep)
                    : std::max(0.0f, mix - bypassFadeStep);
}

void NatAmbioCoreProcessor::blendNaeBypass(const NatAmbioStereoConfig& config,
                                          int numSamples) noexcept
{
    // This runs on fixed NAE frames before the adapter adds its own delay.
    const auto writeOffset = static_cast<size_t>(naeDryWriteFrame) * blockSize;
    std::copy_n(sanitized_input_left.data(), numSamples, naeDryHistoryLeft.data() + writeOffset);
    std::copy_n(sanitized_input_right.data(), numSamples, naeDryHistoryRight.data() + writeOffset);

    const int writtenFrame = naeDryWriteFrame;
    naeDryWriteFrame = (naeDryWriteFrame + 1) % ICORRL;

    if (!config.nae_bypassed && naeBypassMix == 0.0f)
        return;

    const int delayFrames = nae_processor->getLatencySamples(config.nae.steps_length) / blockSize;
    const int readFrame = (writtenFrame + ICORRL - delayFrames) % ICORRL;
    const auto readOffset = static_cast<size_t>(readFrame) * blockSize;
    const auto* dryLeft = naeDryHistoryLeft.data() + readOffset;
    const auto* dryRight = naeDryHistoryRight.data() + readOffset;
    for (int i = 0; i < numSamples; ++i) {
        naeBypassMix = advanceBypassMix(naeBypassMix, config.nae_bypassed);
        if (naeBypassMix == 1.0f) {
            std::copy_n(dryLeft + i, numSamples - i, nae_left_out.data() + i);
            std::copy_n(dryRight + i, numSamples - i, nae_right_out.data() + i);
            break;
        }
        nae_left_out[i] = std::lerp(nae_left_out[i], dryLeft[i], naeBypassMix);
        nae_right_out[i] = std::lerp(nae_right_out[i], dryRight[i], naeBypassMix);
    }
}

void NatAmbioCoreProcessor::fillNaeGainRamps(int numSamples) noexcept
{
    numSamples = std::max(0, std::min(numSamples, blockSize));
    for (int i = 0; i < numSamples; ++i) {
        smoothedNaeGainMain = targetNaeGainMain
            + (smoothedNaeGainMain - targetNaeGainMain) * gainSmoothRetentionPerSample;
        smoothedNaeGainAmb = targetNaeGainAmb
            + (smoothedNaeGainAmb - targetNaeGainAmb) * gainSmoothRetentionPerSample;
        naeGainMainRamp[static_cast<size_t>(i)] = smoothedNaeGainMain;
        naeGainAmbRamp[static_cast<size_t>(i)] = smoothedNaeGainAmb;
    }
}

float NatAmbioCoreProcessor::nextSmoothedXtcAmount() noexcept
{
    const double current = std::isfinite(smoothedXtcAmount)
        ? static_cast<double>(smoothedXtcAmount)
        : static_cast<double>(targetXtcAmount);
    const double next = static_cast<double>(targetXtcAmount)
        + (current - static_cast<double>(targetXtcAmount)) * gainSmoothRetentionPerSample;
    smoothedXtcAmount = static_cast<float>(std::clamp(next, 0.0, 1.0));
    return smoothedXtcAmount;
}

void NatAmbioCoreProcessor::beginPendingXtcWarm()
{
    if (warming_xtc_owner || xtcCrossfadeRemaining > 0)
        return;

    retireFinishedXtcOwner();
    if (fading_xtc_owner)
        return;

    // Keep the steady-state audio path read-only. The exchange is needed only
    // after a producer has published a command; a command arriving after the
    // empty load is intentionally claimed on the next block.
    if (pending_xtc_command.load(std::memory_order_acquire) == kEmptyXtcCommand)
        return;

    const auto command = pending_xtc_command.exchange(kEmptyXtcCommand,
                                                       std::memory_order_acquire);
    if (command == kEmptyXtcCommand)
        return;

    if (command == kDisableXtcCommand) {
        if (active_xtc_owner) {
            fading_xtc_owner = std::move(active_xtc_owner);
            xtcCrossfadeRemaining = xtcCrossfadeLength;
        }
        return;
    }

    warming_xtc_owner.reset(xtcProcessorFromCommand(command));
    if (active_xtc_owner && warming_xtc_owner->copyInputHistoryFrom(*active_xtc_owner)) {
        installWarmedXtcOwner();
        return;
    }

    // A different partition layout needs time-domain history to prime it.
    warming_xtc_owner->clearDelayBuffers();
    xtcWarmBacklogReadPosition = 0;
    xtcWarmBacklogWritePosition = 0;
    xtcWarmBacklogSamples = 0;

    const int historyCapacity = static_cast<int>(xtcHistoryLeft.size());
    const int backlogCapacity = static_cast<int>(xtcWarmBacklogLeft.size());
    if (historyCapacity <= 0 || backlogCapacity <= 0)
        return;

    const auto pushSample = [&](float left, float right) {
        if (xtcWarmBacklogSamples >= backlogCapacity)
            return;
        const auto index = static_cast<size_t>(xtcWarmBacklogWritePosition);
        xtcWarmBacklogLeft[index] = left;
        xtcWarmBacklogRight[index] = right;
        xtcWarmBacklogWritePosition = (xtcWarmBacklogWritePosition + 1) % backlogCapacity;
        ++xtcWarmBacklogSamples;
    };

    const int retainedSamples = xtcHistorySamples;
    const int retainedRemainder = retainedSamples % blockSize;
    const int historyStartPhase =
        (xtcHistoryStreamPhase - retainedRemainder + blockSize) % blockSize;
    for (int i = 0; i < historyStartPhase; ++i)
        pushSample(0.0f, 0.0f);

    int readPosition = xtcHistoryWritePosition - retainedSamples;
    if (readPosition < 0)
        readPosition += historyCapacity;
    for (int i = 0; i < retainedSamples; ++i) {
        const auto source = static_cast<size_t>(readPosition);
        pushSample(xtcHistoryLeft[source], xtcHistoryRight[source]);
        readPosition = (readPosition + 1) % historyCapacity;
    }
}

void NatAmbioCoreProcessor::appendWarmingXtcInput(const float* left,
                                                  const float* right,
                                                  int numSamples) noexcept
{
    const int capacity = static_cast<int>(xtcWarmBacklogLeft.size());
    if (!warming_xtc_owner || capacity <= 0)
        return;

    for (int i = 0; i < numSamples && xtcWarmBacklogSamples < capacity; ++i) {
        const auto index = static_cast<size_t>(xtcWarmBacklogWritePosition);
        xtcWarmBacklogLeft[index] = left[i];
        xtcWarmBacklogRight[index] = right[i];
        xtcWarmBacklogWritePosition = (xtcWarmBacklogWritePosition + 1) % capacity;
        ++xtcWarmBacklogSamples;
    }
}

void NatAmbioCoreProcessor::advanceWarmingXtc(const XtcStereoConfig& config,
                                              int numSamples,
                                              bool allowInstall)
{
    if (!warming_xtc_owner)
        return;

    const int capacity = static_cast<int>(xtcWarmBacklogLeft.size());
    int budget = std::min(xtcWarmBacklogSamples,
                          kXtcWarmWorkMultiplier * std::max(0, numSamples));
    XtcStereoConfig warmConfig = config;
    warmConfig.amount = 1.0f;

    while (budget > 0 && xtcWarmBacklogSamples > 0) {
        const int chunk = std::min({blockSize, budget, xtcWarmBacklogSamples});
        for (int i = 0; i < chunk; ++i) {
            const auto source = static_cast<size_t>(xtcWarmBacklogReadPosition);
            xtcWarmInputLeft[static_cast<size_t>(i)] = xtcWarmBacklogLeft[source];
            xtcWarmInputRight[static_cast<size_t>(i)] = xtcWarmBacklogRight[source];
            xtcWarmBacklogReadPosition = (xtcWarmBacklogReadPosition + 1) % capacity;
        }
        xtcWarmBacklogSamples -= chunk;
        budget -= chunk;
        warming_xtc_owner->process(xtcWarmInputLeft.data(), xtcWarmInputRight.data(),
                                   xtcWarmOutputLeft.data(), xtcWarmOutputRight.data(),
                                   chunk, warmConfig);
    }

    if (xtcWarmBacklogSamples == 0 && allowInstall)
        installWarmedXtcOwner();
}

void NatAmbioCoreProcessor::installWarmedXtcOwner() noexcept
{
    if (!warming_xtc_owner)
        return;

    if (active_xtc_owner) {
        fading_xtc_owner = std::move(active_xtc_owner);
        xtcCrossfadeRemaining = xtcCrossfadeLength;
    } else {
        xtcCrossfadeRemaining = 0;
    }
    active_xtc_owner = std::move(warming_xtc_owner);
    xtcWarmBacklogReadPosition = 0;
    xtcWarmBacklogWritePosition = 0;
    xtcWarmBacklogSamples = 0;
}

void NatAmbioCoreProcessor::retireFinishedXtcOwner() noexcept
{
    if (!fading_xtc_owner || xtcCrossfadeRemaining > 0)
        return;

    auto* empty = static_cast<XtcProcessor*>(nullptr);
    auto* finished = fading_xtc_owner.get();
    if (retired_xtc_owner.compare_exchange_strong(empty,
                                                   finished,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed))
        (void) fading_xtc_owner.release();
}

void NatAmbioCoreProcessor::reclaimRetiredXtcOwner() noexcept
{
    // Called only from producer/lifecycle code, never from process().
    std::unique_ptr<XtcProcessor> retired(
        retired_xtc_owner.exchange(nullptr, std::memory_order_acq_rel));
}

void NatAmbioCoreProcessor::discardPendingXtcCommand() noexcept
{
    // Called only while audio is stopped. Disable carries no heap ownership.
    const auto command = pending_xtc_command.exchange(kEmptyXtcCommand,
                                                       std::memory_order_acq_rel);
    destroyXtcProcessorCommand(command);
}

void NatAmbioCoreProcessor::clearXtcOwnershipOnStoppedThread() noexcept
{
    discardPendingXtcCommand();
    reclaimRetiredXtcOwner();
    warming_xtc_owner.reset();
    fading_xtc_owner.reset();
    active_xtc_owner.reset();
}

void NatAmbioCoreProcessor::warmXtcProcessor(XtcProcessor& processor,
                                             const XtcStereoConfig& config)
{
    const int capacity = static_cast<int>(xtcHistoryLeft.size());
    const int retainedSamples = xtcHistorySamples;
    if (capacity <= 0 || retainedSamples <= 0)
        return;

    XtcStereoConfig warmConfig = config;
    warmConfig.amount = 1.0f;

    const int retainedRemainder = retainedSamples % blockSize;
    const int historyStartPhase =
        (xtcHistoryStreamPhase - retainedRemainder + blockSize) % blockSize;
    if (historyStartPhase > 0) {
        std::fill_n(xtcWarmInputLeft.data(), historyStartPhase, 0.0f);
        std::fill_n(xtcWarmInputRight.data(), historyStartPhase, 0.0f);
        processor.process(xtcWarmInputLeft.data(), xtcWarmInputRight.data(),
                          xtcWarmOutputLeft.data(), xtcWarmOutputRight.data(),
                          historyStartPhase, warmConfig);
    }

    int readPosition = xtcHistoryWritePosition - retainedSamples;
    if (readPosition < 0)
        readPosition += capacity;

    int remaining = retainedSamples;
    while (remaining > 0) {
        const int chunk = std::min(blockSize, remaining);
        for (int i = 0; i < chunk; ++i) {
            const int source = (readPosition + i) % capacity;
            xtcWarmInputLeft[static_cast<size_t>(i)] = xtcHistoryLeft[static_cast<size_t>(source)];
            xtcWarmInputRight[static_cast<size_t>(i)] = xtcHistoryRight[static_cast<size_t>(source)];
        }

        processor.process(xtcWarmInputLeft.data(), xtcWarmInputRight.data(),
                          xtcWarmOutputLeft.data(), xtcWarmOutputRight.data(),
                          chunk, warmConfig);
        readPosition = (readPosition + chunk) % capacity;
        remaining -= chunk;
    }
}

void NatAmbioCoreProcessor::appendXtcHistory(const float* left,
                                             const float* right,
                                             int numSamples) noexcept
{
    const int capacity = static_cast<int>(xtcHistoryLeft.size());
    if (capacity <= 0)
        return;

    for (int i = 0; i < numSamples; ++i) {
        xtcHistoryLeft[static_cast<size_t>(xtcHistoryWritePosition)] = left[i];
        xtcHistoryRight[static_cast<size_t>(xtcHistoryWritePosition)] = right[i];
        xtcHistoryWritePosition = (xtcHistoryWritePosition + 1) % capacity;
        xtcHistorySamples = std::min(capacity, xtcHistorySamples + 1);
    }
    xtcHistoryStreamPhase =
        (xtcHistoryStreamPhase + numSamples % blockSize) % blockSize;
}

void NatAmbioCoreProcessor::reset()
{
    if (nae_processor)
        nae_processor->reset();
    if (fading_nae_processor)
        fading_nae_processor->reset();
    if (active_xtc_owner)
        active_xtc_owner->reset();
    if (fading_xtc_owner)
        fading_xtc_owner->reset();

    if (warming_xtc_owner)
        warming_xtc_owner->reset();
    // Abort any pending crossfade on full reset. reset() can be reached from a
    // realtime host callback, so completed ownership is returned rather than
    // destroyed here. A full retirement slot deliberately retains fading as
    // bounded backpressure until producer/lifecycle reclamation.
    fadingNaeActive = false;
    naeCrossfadeRemaining = 0;
    xtcCrossfadeRemaining = 0;
    retireFinishedXtcOwner();
    lastEffectiveNaeConfigValid = false;
    lastXtcProcessingEnabled = false;
    xtcHistoryWritePosition = 0;
    xtcHistorySamples = 0;
    xtcHistoryStreamPhase = 0;
    xtcWarmBacklogReadPosition = 0;
    xtcWarmBacklogWritePosition = 0;
    xtcWarmBacklogSamples = 0;
    std::fill(xtcWarmBacklogLeft.begin(), xtcWarmBacklogLeft.end(), 0.0f);
    std::fill(xtcWarmBacklogRight.begin(), xtcWarmBacklogRight.end(), 0.0f);

    std::fill(xtcHistoryLeft.begin(), xtcHistoryLeft.end(), 0.0f);
    std::fill(xtcHistoryRight.begin(), xtcHistoryRight.end(), 0.0f);

    smoothedNaeGainMain = targetNaeGainMain;
    smoothedNaeGainAmb = targetNaeGainAmb;
    smoothedXtcAmount = targetXtcAmount;
    bypassMixInitialized = false;

    resetBlockAdapter();
    fixedFrameAdapterActive = false;
}

int NatAmbioCoreProcessor::getLatencySamples(const NatAmbioStereoConfig& config) const
{
    int latency = 0;
    if (config.nae_enabled)
        latency += blockSize;
    if (nae_processor && config.nae_enabled)
        latency += nae_processor->getLatencySamples(config.nae.steps_length);

    return latency;
}

float NatAmbioCoreProcessor::sanitizeInputSample(float value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;
    return value;
}

float NatAmbioCoreProcessor::sanitizeOutputSample(float value) noexcept
{
    if (!std::isfinite(value))
        return 0.0f;
    return value;
}

int NatAmbioCoreProcessor::clampNaeSteps(int stepsLength) const noexcept
{
    return std::clamp(stepsLength / std::max(1, blockSize), 1, ICORRL);
}

void NatAmbioCoreProcessor::sanitizeInputBlock(const float* left_in, const float* right_in, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i) {
        sanitized_input_left[static_cast<size_t>(i)] = sanitizeInputSample(left_in[i]);
        sanitized_input_right[static_cast<size_t>(i)] = sanitizeInputSample(right_in[i]);
    }
}

void NatAmbioCoreProcessor::sanitizeOutputBlock(float* left_out, float* right_out, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i) {
        left_out[i] = sanitizeOutputSample(left_out[i]);
        right_out[i] = sanitizeOutputSample(right_out[i]);
    }
}

void NatAmbioCoreProcessor::resetBlockAdapter() noexcept
{
    std::fill(naeDryHistoryLeft.begin(), naeDryHistoryLeft.end(), 0.0f);
    std::fill(naeDryHistoryRight.begin(), naeDryHistoryRight.end(), 0.0f);
    naeDryWriteFrame = 0;
    inputFrameFill = 0;
    outputFifoReadPosition = 0;
    outputFifoWritePosition = 0;
    outputFifoSamples = 0;

    std::fill(buffered_input_left.begin(), buffered_input_left.end(), 0.0f);
    std::fill(buffered_input_right.begin(), buffered_input_right.end(), 0.0f);
    std::fill(buffered_output_left.begin(), buffered_output_left.end(), 0.0f);
    std::fill(buffered_output_right.begin(), buffered_output_right.end(), 0.0f);
    std::fill(buffered_nae_tap_left.begin(), buffered_nae_tap_left.end(), 0.0f);
    std::fill(buffered_nae_tap_right.begin(), buffered_nae_tap_right.end(), 0.0f);
    std::fill(output_fifo_left.begin(), output_fifo_left.end(), 0.0f);
    std::fill(output_fifo_right.begin(), output_fifo_right.end(), 0.0f);
    std::fill(nae_tap_fifo_left.begin(), nae_tap_fifo_left.end(), 0.0f);
    std::fill(nae_tap_fifo_right.begin(), nae_tap_fifo_right.end(), 0.0f);
}

void NatAmbioCoreProcessor::pushOutputSamples(const float* left, const float* right,
                                              const float* nae_tap_left, const float* nae_tap_right,
                                              int numSamples) noexcept
{
    const int capacity = static_cast<int>(output_fifo_left.size());
    if (capacity <= 0)
        return;

    for (int i = 0; i < numSamples; ++i) {
        if (outputFifoSamples == capacity) {
            outputFifoReadPosition = (outputFifoReadPosition + 1) % capacity;
            --outputFifoSamples;
        }

        output_fifo_left[static_cast<size_t>(outputFifoWritePosition)] = left[i];
        output_fifo_right[static_cast<size_t>(outputFifoWritePosition)] = right[i];
        nae_tap_fifo_left[static_cast<size_t>(outputFifoWritePosition)] =
            nae_tap_left != nullptr ? nae_tap_left[i] : left[i];
        nae_tap_fifo_right[static_cast<size_t>(outputFifoWritePosition)] =
            nae_tap_right != nullptr ? nae_tap_right[i] : right[i];
        outputFifoWritePosition = (outputFifoWritePosition + 1) % capacity;
        ++outputFifoSamples;
    }
}

int NatAmbioCoreProcessor::popOutputSamples(float* left, float* right,
                                            float* nae_tap_left, float* nae_tap_right,
                                            int numSamples) noexcept
{
    const int capacity = static_cast<int>(output_fifo_left.size());
    if (capacity <= 0)
        return 0;

    int popped = 0;
    while (popped < numSamples && outputFifoSamples > 0) {
        left[popped] = output_fifo_left[static_cast<size_t>(outputFifoReadPosition)];
        right[popped] = output_fifo_right[static_cast<size_t>(outputFifoReadPosition)];
        if (nae_tap_left != nullptr)
            nae_tap_left[popped] = nae_tap_fifo_left[static_cast<size_t>(outputFifoReadPosition)];
        if (nae_tap_right != nullptr)
            nae_tap_right[popped] = nae_tap_fifo_right[static_cast<size_t>(outputFifoReadPosition)];
        outputFifoReadPosition = (outputFifoReadPosition + 1) % capacity;
        --outputFifoSamples;
        ++popped;
    }

    return popped;
}
