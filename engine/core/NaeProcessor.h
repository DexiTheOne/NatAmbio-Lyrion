/*
 * NAE (Ambient Extractor) Processor for VST3 Plugin
 *
 * UPSTREAM REFERENCE:
 * - Repository: https://github.com/raul-fernandez-ortega/natambio
 * - Commit: 6723da8807807aacc71b5d60610dc4f6b808a120
 * - Source file: src/nae.cpp
 * 
 * The PCA (Principal Component Analysis) algorithm for stereo ambient extraction 
 * is derived from the upstream implementation. This VST3 version:
 * - Removes threading (runs synchronously)
 * - Removes JACK I/O (uses direct buffer pointers)
 * - Simplifies to stereo-only mode
 * - Pre-allocates all buffers in prepare()
 */

#ifndef NAE_PROCESSOR_H
#define NAE_PROCESSOR_H

#include "NatAmbioConstants.h"
#include <algorithm>
#include <vector>
#include <cmath>
#include <memory>

struct NaeProcessorTestAccess;

/**
 * @brief NAE Processor - PCA-based Ambient Extractor for stereo audio.
 * 
 * Extracts ambient (surround) content from stereo input using Principal Component Analysis.
 * 
 * Algorithm (from upstream):
 * 1. Accumulate covariance matrix from stereo samples
 * 2. Compute eigenvalues/eigenvectors via 2x2 eigen decomposition
 * 3. Apply PCA transform to decompose into mid/side components
 * 4. Output front main+ambient, or rear surround from the secondary component
 * 
 * Real-time Safety:
 * - All buffers pre-allocated in prepare()
 * - No allocations in process()
 * - No locks or waits
 * - No exceptions
 */
class NaeProcessor {
public:
    struct PcaAnalysis
    {
        bool valid = false;
        std::uint64_t generation = 0;
        double covMidMid = 0.0;
        double covMidSide = 0.0;
        double covSideSide = 0.0;
        double eigenvaluePrimary = 0.0;
        double eigenvalueSecondary = 0.0;
        double principalMid = 1.0;
        double principalSide = 0.0;
        int windowSamples = 0;
        NaeMode mode = NaeMode::Front;
    };

    /**
     * @brief Create a new NAE processor.
     * @param sampleRate Audio sample rate (44100, 48000, etc.)
     * @param blockSize Audio block size in samples
     */
    NaeProcessor(int sampleRate, int blockSize);
    
    /**
     * @brief Prepare for playback.
     * 
     * Pre-allocates all buffers and initializes state.
     * Must be called before any process() calls.
     * 
     * @param sampleRate Audio sample rate in Hz
     * @param blockSize Audio block size in samples
     */
    void prepare(int sampleRate, int blockSize);
    
    /**
     * @brief Process stereo audio to extract ambient content.
     * 
     * Input: Stereo audio (left, right channels)
     * Output: Processed outputs based on configuration
     * 
     * If NAE is disabled, outputs pass through unchanged.
     * If NAE is enabled, outputs are PCA-transformed signals.
     * 
     * @param left_in Left channel input buffer
     * @param right_in Right channel input buffer
     * @param left_out Left channel output buffer
     * @param right_out Right channel output buffer
     * @param numSamples Number of samples to process (<= prepared blockSize)
     * @param config NAE configuration (enabled, gains, window length)
     */
    void process(const float* left_in, const float* right_in,
                 float* left_out, float* right_out,
                 int numSamples,
                 const NaeStereoConfig& config);

    // Core-only automation path: each optional array contains a dB value for
    // every output sample. Null arrays preserve the scalar config behavior used
    // by source-parity callers.
    void processWithGainRamps(const float* left_in, const float* right_in,
                              float* left_out, float* right_out,
                              int numSamples, const NaeStereoConfig& config,
                              const double* gainMainDb,
                              const double* gainAmbDb);

    void process(const float* left_in, const float* right_in,
                 float* left_out, float* right_out,
                 const NaeStereoConfig& config);
    
    /**
     * @brief Reset internal state (covariance matrix, accumulators).
     * 
     * Should be called when seeking or starting a new performance.
     */
    void reset();
    void copyStateFrom(const NaeProcessor& other) noexcept;
    PcaAnalysis getPcaAnalysis() const noexcept { return pcaAnalysis; }
    void setPcaReporting(bool enabled) noexcept
    {
        pcaReporting = enabled;
        if (!enabled)
            pcaAnalysis = {};
    }
    
    /**
     * @brief Get latency contribution from NAE (in samples).
     * @return Delay from upstream-style covsteps history output.
     */
    int getLatencySamples() const
    {
        return getLatencySamples(2048);
    }

    int getLatencySamples(int stepsLength) const
    {
        const int safeBlockSize = std::max(1, blockSize);
        const int steps = std::clamp(stepsLength / safeBlockSize, 1, ICORRL);
        return (steps - 1) * safeBlockSize;
    }

private:
    friend struct NaeProcessorTestAccess;

    // Configuration
    int sampleRate = 48000;
    int blockSize = 256;
    
    // Covariance matrix accumulators (from upstream src/nae.cpp)
    struct CovarianceAccumulator {
        std::unique_ptr<double[]> sum_xy_array;      // Σ(x·y)
        std::unique_ptr<double[]> sum_x2_array;      // Σ(x²)
        std::unique_ptr<double[]> sum_y2_array;      // Σ(y²)
        std::unique_ptr<double[]> sum_x_array;       // Σ(x)
        std::unique_ptr<double[]> sum_y_array;       // Σ(y)
    } covariance, correlation;
    
    // PCA transform outputs (from upstream src/nae.cpp)
    struct PcaTransform {
        std::unique_ptr<double[]> mid_step;
        std::unique_ptr<double[]> side_step;
        std::unique_ptr<double[]> mid_left;
        std::unique_ptr<double[]> mid_right;
        std::unique_ptr<double[]> side_left;
        std::unique_ptr<double[]> side_right;
    } pca;
    
    int active_steps = 0;
    NaeMode active_mode = NaeMode::Front;
    bool active_mode_valid = false;
    
    // Eigen decomposition results
    double eig1 = 1.0, eig2 = 0.0;             // Eigenvalues
    double eig1_v1[2] = {1.0, 0.0};            // Eigenvector 1
    double eig1_v2[2] = {0.0, 1.0};            // Eigenvector 2
    PcaAnalysis pcaAnalysis;
    bool pcaReporting = true;
    
    /**
     * @brief Compute 2x2 symmetric eigen decomposition.
     * 
     * From upstream src/nae.cpp: eigen_2x2_symmetric()
     * Solves the 2x2 symmetric eigenvalue problem for PCA.
     * 
     * @param a Element [0,0] of symmetric matrix
     * @param b Element [0,1] of symmetric matrix
     * @param d Element [1,1] of symmetric matrix
     * @param eig1 Output: First eigenvalue
     * @param eig2 Output: Second eigenvalue
     * @param v1 Output: First eigenvector [2]
     * @param v2 Output: Second eigenvector [2]
     * @return 0 on success
     */
    int eigen_2x2_symmetric(double a, double b, double d,
                            double* eig1, double* eig2,
                            double v1[2], double v2[2]) const;
    
    /**
     * @brief Accumulate covariance data from stereo samples.
     * 
     * From upstream src/nae.cpp: Accumulates the covariance matrix
     * for PCA analysis over steps_length samples.
     * 
     * @param left Left channel samples
     * @param right Right channel samples
     * @param num_samples Number of samples to process
     * @param window_length Covariance window length (steps_length)
     * @param pan Side-channel scale (1.0 front, correlation-derived rear)
     */
    void accumulateCovariance(const float* left, const float* right,
                              int num_samples, int window_length, double pan);

    double accumulateRearCorrelation(const float* left, const float* right,
                                     int num_samples);
    void shiftRearCorrelation() noexcept;
    
    /**
     * @brief Apply PCA transform using computed eigenvectors.
     * 
     * From upstream src/nae.cpp: Transforms stereo input to mid/side
     * representation using the computed PCA eigenvectors.
     * 
     * @param left Input left channel
     * @param right Input right channel
     * @param num_samples Number of samples
     * @param config NAE configuration (gains)
     * @param left_out Output left channel
     * @param right_out Output right channel
     */
    void applyPcaTransform(const float* left, const float* right,
                           int num_samples, const NaeStereoConfig& config,
                           float* left_out, float* right_out,
                           const double* gainMainDb,
                           const double* gainAmbDb);

    int clampSteps(int window_length) const;
    void reconcileWindowSteps(int new_steps, int num_samples);
    void clearHistorySlot(int slot, int num_samples);
    void copyHistorySlot(int dstSlot, int srcSlot, int num_samples);
    void clearCovarianceSlot(int slot);
    void copyCovarianceSlot(int dstSlot, int srcSlot);
};

#endif  // NAE_PROCESSOR_H
