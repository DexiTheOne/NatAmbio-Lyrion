/*
 * PartitionedConvolver.h
 *
 * JUCE-free partitioned FFT convolver for efficient runtime application of
 * long XTC FIR filters.
 *
 * Goal: Allow full reference-length filters (e.g. 4k-8k+ taps) at 96 kHz+ with
 * low CPU and low added latency, while keeping NAE/XTC output behavior
 * identical to the direct/reference implementation.
 *
 * Design:
 * - Overlap-Save (or Overlap-Add) partitioned convolution.
 * - Pre-FFT the filter once (off audio thread, in configure/prepare).
 * - Partition size chosen for low latency (e.g. 256-512 samples) and good perf.
 * - Uses the same FFT primitives as XtcFilterDesigner for consistency where possible.
 *
 * This replaces the direct time-domain path in XtcProcessor for the hot path.
 * Filter generation (XtcFilterDesigner) remains untouched for reference parity.
 *
 * For long-running realtime use, SR-aware filter lengths and smoothing keep
 * the implementation responsive, correct, and efficient.
 */

#ifndef PARTITIONED_CONVOLVER_H
#define PARTITIONED_CONVOLVER_H

#include "NatAmbioConstants.h"
#include <vector>
#include <complex>
#include <memory>

namespace NatAmbio {

class PartitionedConvolver {
public:
    PartitionedConvolver();
    ~PartitionedConvolver() = default;

    // Prepare with filter (full length from reference design) and desired partition size.
    // partitionSize should be power of 2, e.g. 256 or 512 for low latency.
    // Must be called off audio thread (like current configure).
    void prepare(const float* filter, int filterLength, int partitionSize, int sampleRate);

    // Process an arbitrary-sized block with zero added algorithmic latency.
    // Input/output may refer to the same buffer.
    void process(const float* input, float* output, int numSamples);

    // Transfer compatible input history and recompute the filter-dependent overlap.
    bool copyInputHistoryFrom(const PartitionedConvolver& source) noexcept;

    void reset();

    int getLatencySamples() const { return latencySamples; }

private:
    void fft(std::vector<std::complex<float>>& a, bool inverse);
    void processPendingInput(float* output, int outputStart, int numOutput);
    void convolveSpectrum(const std::complex<float>* currentSpectrum, int currentPosition);
    void updateOverlap();

    int partitionSize = 0;
    int fftSize = 0;
    int filterLen = 0;
    int partitionCount = 0;
    int latencySamples = 0;
    int writePos = 0;
    int pendingCount = 0;
    bool prepared = false;

    std::vector<std::complex<float>> filterPartitions;
    std::vector<std::complex<float>> inputSpectra;
    std::vector<std::complex<float>> accumSpectrum;
    std::vector<float> overlap;
    std::vector<float> pendingInput;
    std::vector<std::complex<float>> fftWork;
};

} // namespace NatAmbio

#endif // PARTITIONED_CONVOLVER_H
