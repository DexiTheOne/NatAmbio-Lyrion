/*
 * XTC Processor Implementation
 *
 * Binaural filtering algorithm adapted from upstream natambio lib/xtc.c
 * Reference: https://github.com/raul-fernandez-ortega/natambio (commit 6723da8)
 */

#include "XtcProcessor.h"
#include <cstring>
#include <algorithm>
#include <cmath>

XtcProcessor::XtcProcessor(int sr, int bs)
    : sampleRate(sr), blockSize(bs)
{
    prepare(sr, bs);
}

void XtcProcessor::prepare(int sr, int bs)
{
    sampleRate = sr;
    blockSize = bs;
    
    // Scratch buffers are allocated once here and reused by process().
    midBlock.assign(static_cast<size_t>(blockSize), 0.0f);
    sideBlock.assign(static_cast<size_t>(blockSize), 0.0f);
    midWet.assign(static_cast<size_t>(blockSize), 0.0f);
    sideWet.assign(static_cast<size_t>(blockSize), 0.0f);
    
    reset();
}

void XtcProcessor::configure(const XtcStereoConfig& config)
{
    generateBinauralFilters(config);
}

void XtcProcessor::reset()
{
    clearDelayBuffers();
}

void XtcProcessor::clearDelayBuffers() noexcept
{
    midConvolver.reset();
    sideConvolver.reset();
}

bool XtcProcessor::copyInputHistoryFrom(const XtcProcessor& source) noexcept
{
    return midConvolver.copyInputHistoryFrom(source.midConvolver)
        && sideConvolver.copyInputHistoryFrom(source.sideConvolver);
}

void XtcProcessor::process(const float* left_in, const float* right_in,
                          float* left_out, float* right_out,
                          int numSamples,
                          const XtcStereoConfig& config)
{
    if (!left_in || !right_in || !left_out || !right_out) {
        return;
    }

    numSamples = std::max(0, std::min(numSamples, blockSize));
    if (numSamples == 0) {
        return;
    }
    
    // If XTC is disabled or amount is 0, pass through unchanged
    if (config.amount <= 0.001f) {
        std::memcpy(left_out, left_in, static_cast<size_t>(numSamples) * sizeof(float));
        std::memcpy(right_out, right_in, static_cast<size_t>(numSamples) * sizeof(float));
        return;
    }
    
    const float amt = std::clamp(config.amount, 0.0f, 1.0f);
    const float dry = 1.0f - amt;

    for (int n = 0; n < numSamples; ++n) {
        const float leftInput = left_in[n];
        const float rightInput = right_in[n];
        midBlock[static_cast<size_t>(n)] = leftInput + rightInput;
        sideBlock[static_cast<size_t>(n)] = leftInput - rightInput;
    }

    midConvolver.process(midBlock.data(), midWet.data(), numSamples);
    sideConvolver.process(sideBlock.data(), sideWet.data(), numSamples);

    for (int n = 0; n < numSamples; ++n) {
        const float leftInput = left_in[n];
        const float rightInput = right_in[n];
        const auto index = static_cast<size_t>(n);

        const float left_wet = 0.5f * (midWet[index] + sideWet[index]);
        const float right_wet = 0.5f * (midWet[index] - sideWet[index]);

        left_out[n] = leftInput * dry + left_wet * amt;
        right_out[n] = rightInput * dry + right_wet * amt;
    }
}

void XtcProcessor::process(const float* left_in, const float* right_in,
                          float* left_out, float* right_out,
                          const XtcStereoConfig& config)
{
    process(left_in, right_in, left_out, right_out, blockSize, config);
}

void XtcProcessor::generateBinauralFilters(const XtcStereoConfig& config)
{
    const XtcDesignedFilters filters = XtcFilterDesigner::design(config, sampleRate);

    const int filterLength = static_cast<int>(filters.direct.size());
    latency_samples = filters.latencySamples;

    // Symmetry permits one full-length convolution for Mid and one for Side.
    std::vector<float> sumFilter(static_cast<size_t>(filterLength), 0.0f);
    std::vector<float> diffFilter(static_cast<size_t>(filterLength), 0.0f);
    for (int i = 0; i < filterLength; ++i) {
        const auto index = static_cast<size_t>(i);
        sumFilter[index] = filters.direct[index] + filters.cross[index];
        diffFilter[index] = filters.direct[index] - filters.cross[index];
    }

    // Prepare the FFT convolvers off the audio thread for the host block size.
    if (filterLength > 0) {
        midConvolver.prepare(sumFilter.data(), filterLength, blockSize, sampleRate);
        sideConvolver.prepare(diffFilter.data(), filterLength, blockSize, sampleRate);
    }
}

int XtcProcessor::getLatencySamples() const
{
    return latency_samples;
}
