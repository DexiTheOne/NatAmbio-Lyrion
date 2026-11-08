/*
 * XTC (Crosstalk Cancellation) Processor for VST3 Plugin
 *
 * UPSTREAM REFERENCE:
 * - Repository: https://github.com/raul-fernandez-ortega/natambio
 * - Commit: 6723da8807807aacc71b5d60610dc4f6b808a120
 * - Source files: src/convchannel.cpp, lib/xtc.c
 * 
 * XTC applies binaural rendering filters to create crosstalk cancellation.
 * The runtime uses uniform partitioned FFT convolution for the full FIR.
 * 
 * Algorithm:
 * 1. Generate filter impulse responses (direct + cross paths)
 * 2. Convolve input stereo signal with filters
 * 3. Sum outputs to create crosstalk-cancelled result
 */

#ifndef XTC_PROCESSOR_H
#define XTC_PROCESSOR_H

#include "NatAmbioConstants.h"
#include "XtcFilterDesigner.h"
#include "PartitionedConvolver.h"
#include <vector>

/**
 * @brief XTC Processor - Crosstalk Cancellation via binaural filtering.
 * 
 * Applies binaural rendering filters to stereo input for crosstalk cancellation
 * based on HRTF measurements and inter-aural time/level differences.
 * 
 * Real-time Safety:
 * - All buffers pre-allocated in prepare()
 * - No allocations in process()
 * - No locks or waits
 * - No exceptions
 * - Uses mid/side partitioned FFT convolution with exact configured tap counts
 *   and arbitrary callback-size streaming.
 */
class XtcProcessor {
public:
    /**
     * @brief Create a new XTC processor.
     * @param sampleRate Audio sample rate (44100, 48000, etc.)
     * @param blockSize Audio block size in samples
     */
    XtcProcessor(int sampleRate, int blockSize);
    
    ~XtcProcessor() = default;
    
    /**
     * @brief Prepare for playback.
     * 
     * Allocates the reusable processing scratch buffers. Call configure()
     * outside the realtime callback before any process() calls.
     * 
     * @param sampleRate Audio sample rate in Hz
     * @param blockSize Audio block size in samples
     */
    void prepare(int sampleRate, int blockSize);

    /**
     * @brief Generate and install XTC FIRs for the supplied configuration.
     *
     * This allocates and must be called outside the realtime audio callback.
     */
    void configure(const XtcStereoConfig& config);
    
    /**
     * @brief Process stereo audio with XTC crosstalk cancellation.
     * 
     * Input: Stereo audio (left, right channels)
     * Output: Crosstalk-cancelled stereo
     * 
     * If XTC is disabled or amount=0, outputs pass through unchanged.
     * Otherwise, applies binaural filters and outputs result.
     * 
     * @param left_in Left channel input buffer
     * @param right_in Right channel input buffer
     * @param left_out Left channel output buffer
     * @param right_out Right channel output buffer
     * @param numSamples Number of samples to process (<= prepared blockSize)
     * @param config XTC configuration (enabled, amount, ITD, ILD, filter length)
     */
    void process(const float* left_in, const float* right_in,
                 float* left_out, float* right_out,
                 int numSamples,
                 const XtcStereoConfig& config);

    void process(const float* left_in, const float* right_in,
                 float* left_out, float* right_out,
                 const XtcStereoConfig& config);
    
    /**
     * @brief Reset internal state (delay buffers, accumulators).
     * 
     * Should be called when seeking or starting a new performance.
     */
    void reset();

    /**
     * @brief Clear runtime delay/history buffers without changing installed FIRs.
     */
    void clearDelayBuffers() noexcept;

    bool copyInputHistoryFrom(const XtcProcessor& source) noexcept;
    
    /**
     * @brief Get latency contribution from XTC (in samples).
     * @return Zero for faithful upstream XTC; the declared linear-phase delay
     *         for the optional bass-preserving treatment.
     */
    int getLatencySamples() const;
    
private:
    // Configuration
    int sampleRate = 48000;
    int blockSize = 256;
    int latency_samples = 0;

    // Partitioned FFT convolvers for mid/side (for full GUI-selected tap counts at low CPU)
    NatAmbio::PartitionedConvolver midConvolver;
    NatAmbio::PartitionedConvolver sideConvolver;
    std::vector<float> midBlock;
    std::vector<float> sideBlock;
    std::vector<float> midWet;
    std::vector<float> sideWet;
    
    /**
     * @brief Generate binaural filter impulse responses.
     * 
     * Creates minimal binaural filters based on:
     * - ITD (Inter-aural Time Difference): delays between channels
     * - ILD (Inter-aural Level Difference): level differences
     * - Azimuth: spatial angle
     * 
     * From upstream lib/xtc.c: XTC filter generation algorithm
     * 
     * @param config XTC configuration
     */
    void generateBinauralFilters(const XtcStereoConfig& config);
};

#endif  // XTC_PROCESSOR_H
