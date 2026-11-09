/*
 * NAE Processor Implementation
 *
 * PCA algorithm adapted from upstream natambio src/nae.cpp
 * Reference: https://github.com/raul-fernandez-ortega/natambio (commit 6723da8)
 */

#include "NaeProcessor.h"
#include <cstring>

namespace
{
constexpr double kRearPanMinimum = 0.55;
constexpr double kRearPanCorrelationScale = 0.45;
}

NaeProcessor::NaeProcessor(int sr, int bs)
    : sampleRate(sr), blockSize(bs)
{
    prepare(sr, bs);
}

void NaeProcessor::prepare(int sr, int bs)
{
    sampleRate = sr;
    blockSize = bs;
    
    // Allocate covariance matrix buffers
    int max_steps = ICORRL;  // From NatAmbioConstants.h: ICORRL = 20
    
    covariance.sum_xy_array = std::make_unique<double[]>(max_steps);
    covariance.sum_x2_array = std::make_unique<double[]>(max_steps);
    covariance.sum_y2_array = std::make_unique<double[]>(max_steps);
    covariance.sum_x_array = std::make_unique<double[]>(max_steps);
    covariance.sum_y_array = std::make_unique<double[]>(max_steps);

    // Rear/beta mode derives its mid/side pan from raw-channel correlation
    // over the fixed 20-block upstream history.
    correlation.sum_xy_array = std::make_unique<double[]>(ICORRL);
    correlation.sum_x2_array = std::make_unique<double[]>(ICORRL);
    correlation.sum_y2_array = std::make_unique<double[]>(ICORRL);
    correlation.sum_x_array = std::make_unique<double[]>(ICORRL);
    correlation.sum_y_array = std::make_unique<double[]>(ICORRL);
    
    // Allocate PCA transform history buffers. Upstream stores covsteps blocks;
    // every stored sample receives repeated PCA contributions before output.
    pca.mid_step = std::make_unique<double[]>(max_steps * bs);
    pca.side_step = std::make_unique<double[]>(max_steps * bs);
    pca.mid_left = std::make_unique<double[]>(max_steps * bs);
    pca.mid_right = std::make_unique<double[]>(max_steps * bs);
    pca.side_left = std::make_unique<double[]>(max_steps * bs);
    pca.side_right = std::make_unique<double[]>(max_steps * bs);
    
    reset();
}

void NaeProcessor::reset()
{
    active_steps = 0;
    active_mode = NaeMode::Front;
    active_mode_valid = false;
    pcaAnalysis = {};
    eig1 = 1.0;
    eig2 = 0.0;
    eig1_v1[0] = 1.0; eig1_v1[1] = 0.0;
    eig1_v2[0] = 0.0; eig1_v2[1] = 1.0;
    
    // Clear covariance buffers
    std::memset(covariance.sum_xy_array.get(), 0, ICORRL * sizeof(double));
    std::memset(covariance.sum_x2_array.get(), 0, ICORRL * sizeof(double));
    std::memset(covariance.sum_y2_array.get(), 0, ICORRL * sizeof(double));
    std::memset(covariance.sum_x_array.get(), 0, ICORRL * sizeof(double));
    std::memset(covariance.sum_y_array.get(), 0, ICORRL * sizeof(double));
    std::memset(correlation.sum_xy_array.get(), 0, ICORRL * sizeof(double));
    std::memset(correlation.sum_x2_array.get(), 0, ICORRL * sizeof(double));
    std::memset(correlation.sum_y2_array.get(), 0, ICORRL * sizeof(double));
    std::memset(correlation.sum_x_array.get(), 0, ICORRL * sizeof(double));
    std::memset(correlation.sum_y_array.get(), 0, ICORRL * sizeof(double));

    const int historySamples = ICORRL * blockSize;
    std::memset(pca.mid_step.get(), 0, historySamples * sizeof(double));
    std::memset(pca.side_step.get(), 0, historySamples * sizeof(double));
    std::memset(pca.mid_left.get(), 0, historySamples * sizeof(double));
    std::memset(pca.mid_right.get(), 0, historySamples * sizeof(double));
    std::memset(pca.side_left.get(), 0, historySamples * sizeof(double));
    std::memset(pca.side_right.get(), 0, historySamples * sizeof(double));
}

void NaeProcessor::copyStateFrom(const NaeProcessor& other) noexcept
{
    if (sampleRate != other.sampleRate || blockSize != other.blockSize)
        return;

    active_steps = other.active_steps;
    active_mode = other.active_mode;
    active_mode_valid = other.active_mode_valid;
    eig1 = other.eig1;
    eig2 = other.eig2;
    eig1_v1[0] = other.eig1_v1[0];
    eig1_v1[1] = other.eig1_v1[1];
    eig1_v2[0] = other.eig1_v2[0];
    eig1_v2[1] = other.eig1_v2[1];
    pcaAnalysis = other.pcaAnalysis;

    std::memcpy(covariance.sum_xy_array.get(), other.covariance.sum_xy_array.get(), ICORRL * sizeof(double));
    std::memcpy(covariance.sum_x2_array.get(), other.covariance.sum_x2_array.get(), ICORRL * sizeof(double));
    std::memcpy(covariance.sum_y2_array.get(), other.covariance.sum_y2_array.get(), ICORRL * sizeof(double));
    std::memcpy(covariance.sum_x_array.get(), other.covariance.sum_x_array.get(), ICORRL * sizeof(double));
    std::memcpy(covariance.sum_y_array.get(), other.covariance.sum_y_array.get(), ICORRL * sizeof(double));
    std::memcpy(correlation.sum_xy_array.get(), other.correlation.sum_xy_array.get(), ICORRL * sizeof(double));
    std::memcpy(correlation.sum_x2_array.get(), other.correlation.sum_x2_array.get(), ICORRL * sizeof(double));
    std::memcpy(correlation.sum_y2_array.get(), other.correlation.sum_y2_array.get(), ICORRL * sizeof(double));
    std::memcpy(correlation.sum_x_array.get(), other.correlation.sum_x_array.get(), ICORRL * sizeof(double));
    std::memcpy(correlation.sum_y_array.get(), other.correlation.sum_y_array.get(), ICORRL * sizeof(double));

    const auto historySamples = static_cast<size_t>(ICORRL * blockSize);
    std::memcpy(pca.mid_step.get(), other.pca.mid_step.get(), historySamples * sizeof(double));
    std::memcpy(pca.side_step.get(), other.pca.side_step.get(), historySamples * sizeof(double));
    std::memcpy(pca.mid_left.get(), other.pca.mid_left.get(), historySamples * sizeof(double));
    std::memcpy(pca.mid_right.get(), other.pca.mid_right.get(), historySamples * sizeof(double));
    std::memcpy(pca.side_left.get(), other.pca.side_left.get(), historySamples * sizeof(double));
    std::memcpy(pca.side_right.get(), other.pca.side_right.get(), historySamples * sizeof(double));
}

void NaeProcessor::process(const float* left_in, const float* right_in,
                           float* left_out, float* right_out,
                           int numSamples,
                           const NaeStereoConfig& config)
{
    processWithGainRamps(left_in, right_in, left_out, right_out,
                         numSamples, config, nullptr, nullptr);
}

void NaeProcessor::processWithGainRamps(const float* left_in, const float* right_in,
                                        float* left_out, float* right_out,
                                        int numSamples,
                                        const NaeStereoConfig& config,
                                        const double* gainMainDb,
                                        const double* gainAmbDb)
{
    if (!left_in || !right_in || !left_out || !right_out) {
        return;
    }

    numSamples = std::max(0, std::min(numSamples, blockSize));
    if (numSamples == 0) {
        return;
    }
    
    if (active_mode_valid && config.mode != active_mode)
        reset();
    active_mode = config.mode;
    active_mode_valid = true;

    const int steps = clampSteps(config.steps_length);
    reconcileWindowSteps(steps, numSamples);

    const bool rearMode = config.mode == NaeMode::Rear;
    const double pan = rearMode
        ? accumulateRearCorrelation(left_in, right_in, numSamples)
        : 1.0;
    
    // Accumulate covariance data
    accumulateCovariance(left_in, right_in, numSamples, config.steps_length, pan);
    
    // Apply PCA transform
    applyPcaTransform(left_in, right_in, numSamples, config, left_out, right_out,
                      gainMainDb, gainAmbDb);

    if (rearMode)
        shiftRearCorrelation();
}

void NaeProcessor::process(const float* left_in, const float* right_in,
                           float* left_out, float* right_out,
                           const NaeStereoConfig& config)
{
    process(left_in, right_in, left_out, right_out, blockSize, config);
}

void NaeProcessor::accumulateCovariance(const float* left, const float* right,
                                       int num_samples, int window_length, double pan)
{
    // Number of blocks in the covariance window.
    int steps = clampSteps(window_length);

    // --- Match upstream nae.cpp thr_process() per-block bucket structure ---
    //
    // The upstream clears bucket[covsteps-1], accumulates the ENTIRE current
    // block into it, builds the covariance matrix from all buckets, then shifts
    // buckets left (oldest drops out).
    //
    // The previous cumulative per-sample stepping gave a completely different
    // window shape and was incorrect.

    // Step 1: Clear newest bucket (will hold current block's statistics)
    covariance.sum_xy_array[steps - 1] = 0.0;
    covariance.sum_x2_array[steps - 1] = 0.0;
    covariance.sum_y2_array[steps - 1] = 0.0;
    covariance.sum_x_array [steps - 1] = 0.0;
    covariance.sum_y_array [steps - 1] = 0.0;

    // Step 2: Accumulate current block (mid/side) into newest bucket and store
    // it in the newest PCA history slot.
    const int newestOffset = (steps - 1) * num_samples;
    for (int i = 0; i < num_samples; ++i) {
        double mid  = left[i] + right[i];
        double side = pan * (left[i] - right[i]);
        const int j = newestOffset + i;

        pca.mid_step[j] = mid;
        pca.side_step[j] = side;

        covariance.sum_xy_array[steps - 1] += mid * side;
        covariance.sum_x2_array[steps - 1] += mid * mid;
        covariance.sum_y2_array[steps - 1] += side * side;
        covariance.sum_x_array [steps - 1] += mid;
        covariance.sum_y_array [steps - 1] += side;
    }

    // Step 3: Sum all buckets (sliding window of steps blocks)
    double sum_xy = 0.0, sum_x2 = 0.0, sum_y2 = 0.0;
    double sum_x  = 0.0, sum_y  = 0.0;
    for (int i = 0; i < steps; ++i) {
        sum_xy += covariance.sum_xy_array[i];
        sum_x2 += covariance.sum_x2_array[i];
        sum_y2 += covariance.sum_y2_array[i];
        sum_x  += covariance.sum_x_array [i];
        sum_y  += covariance.sum_y_array [i];
    }

    // Step 4: Build 2×2 covariance matrix.
    // Upstream formula: cov[0][0] = (sum_x2 - sum_x^2/N) / (N-1)
    const double N = static_cast<double>(steps * num_samples);
    if (N > 1.0) {
        double cov_xx = (sum_x2 - sum_x * sum_x / N) / (N - 1.0);
        double cov_xy = (sum_xy - sum_x * sum_y / N) / (N - 1.0);
        double cov_yy = (sum_y2 - sum_y * sum_y / N) / (N - 1.0);

        eigen_2x2_symmetric(cov_xx, cov_xy, cov_yy, &eig1, &eig2, eig1_v1, eig1_v2);

        if (!pcaReporting)
            return;

        const bool finite = std::isfinite(cov_xx) && std::isfinite(cov_xy)
            && std::isfinite(cov_yy) && std::isfinite(eig1) && std::isfinite(eig2)
            && std::isfinite(eig1_v1[0]) && std::isfinite(eig1_v1[1]);
        pcaAnalysis.valid = finite && eig1 >= 0.0 && eig2 >= -1.0e-12;
        if (pcaAnalysis.valid)
        {
            ++pcaAnalysis.generation;
            pcaAnalysis.covMidMid = cov_xx;
            pcaAnalysis.covMidSide = cov_xy;
            pcaAnalysis.covSideSide = cov_yy;
            pcaAnalysis.eigenvaluePrimary = std::max(0.0, eig1);
            pcaAnalysis.eigenvalueSecondary = std::max(0.0, eig2);
            pcaAnalysis.principalMid = eig1_v1[0];
            pcaAnalysis.principalSide = eig1_v1[1];
            pcaAnalysis.windowSamples = steps * num_samples;
            pcaAnalysis.mode = active_mode;
        }
    }
}

double NaeProcessor::accumulateRearCorrelation(const float* left, const float* right,
                                               int num_samples)
{
    const int newest = ICORRL - 1;
    correlation.sum_xy_array[newest] = 0.0;
    correlation.sum_x2_array[newest] = 0.0;
    correlation.sum_y2_array[newest] = 0.0;
    correlation.sum_x_array[newest] = 0.0;
    correlation.sum_y_array[newest] = 0.0;

    for (int i = 0; i < num_samples; ++i) {
        const double leftSample = left[i];
        const double rightSample = right[i];
        correlation.sum_xy_array[newest] += leftSample * rightSample;
        correlation.sum_x2_array[newest] += leftSample * leftSample;
        correlation.sum_y2_array[newest] += rightSample * rightSample;
        correlation.sum_x_array[newest] += leftSample;
        correlation.sum_y_array[newest] += rightSample;
    }

    double sum_xy = 0.0;
    double sum_x2 = 0.0;
    double sum_y2 = 0.0;
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (int i = 0; i < ICORRL; ++i) {
        sum_xy += correlation.sum_xy_array[i];
        sum_x2 += correlation.sum_x2_array[i];
        sum_y2 += correlation.sum_y2_array[i];
        sum_x += correlation.sum_x_array[i];
        sum_y += correlation.sum_y_array[i];
    }

    const double count = static_cast<double>(ICORRL) * static_cast<double>(num_samples);
    const double varianceLeft = count * sum_x2 - sum_x * sum_x;
    const double varianceRight = count * sum_y2 - sum_y * sum_y;

    double absoluteCorrelation = 1.0;
    if (varianceLeft > 0.0 && varianceRight > 0.0) {
        absoluteCorrelation = std::abs(count * sum_xy - sum_x * sum_y)
            / std::sqrt(varianceLeft * varianceRight);
    }

    return kRearPanMinimum + absoluteCorrelation * kRearPanCorrelationScale;
}

void NaeProcessor::shiftRearCorrelation() noexcept
{
    for (int i = 0; i < ICORRL - 1; ++i) {
        correlation.sum_xy_array[i] = correlation.sum_xy_array[i + 1];
        correlation.sum_x2_array[i] = correlation.sum_x2_array[i + 1];
        correlation.sum_y2_array[i] = correlation.sum_y2_array[i + 1];
        correlation.sum_x_array[i] = correlation.sum_x_array[i + 1];
        correlation.sum_y_array[i] = correlation.sum_y_array[i + 1];
    }
}

void NaeProcessor::applyPcaTransform(const float* left, const float* right,
                                     int num_samples, const NaeStereoConfig& config,
                                     float* left_out, float* right_out,
                                     const double* gainMainDb,
                                     const double* gainAmbDb)
{
    (void) left;
    (void) right;

    {
        int steps = clampSteps(config.steps_length);

        const int historySamples = steps * num_samples;
        const double norm_covsteps = static_cast<double>(steps + 1);

        if (config.mode == NaeMode::Rear) {
            for (int i = 0; i < historySamples; ++i) {
                const double side_score = eig1_v2[0] * pca.mid_step[i]
                    + eig1_v2[1] * pca.side_step[i];
                pca.side_left[i] += side_score * eig1_v2[0];
                pca.side_right[i] += side_score * eig1_v2[1];
            }

            const double scalarGainSurr = NatAmbioConstants::db_to_linear(config.gain_surr);
            for (int i = 0; i < num_samples; ++i) {
                const double left_surr = (pca.side_left[i] + pca.side_right[i]) / norm_covsteps;
                const double right_surr = (pca.side_left[i] - pca.side_right[i]) / norm_covsteps;
                const double gainSurr = gainMainDb != nullptr
                    ? NatAmbioConstants::db_to_linear(gainMainDb[i]) : scalarGainSurr;
                left_out[i] = static_cast<float>(gainSurr * left_surr);
                right_out[i] = static_cast<float>(gainSurr * right_surr);
            }
        } else {
            for (int i = 0; i < historySamples; ++i) {
                const double mid = pca.mid_step[i];
                const double side = pca.side_step[i];

                const double main_score = eig1_v1[0] * mid + eig1_v1[1] * side;
                const double amb_score = eig1_v2[0] * mid + eig1_v2[1] * side;

                pca.mid_left[i] += main_score * eig1_v1[0];
                pca.mid_right[i] += main_score * eig1_v1[1];
                pca.side_left[i] += amb_score * eig1_v2[0];
                pca.side_right[i] += amb_score * eig1_v2[1];
            }

            const double scalarGainMain = NatAmbioConstants::db_to_linear(config.gain_main);
            const double scalarGainAmb = NatAmbioConstants::db_to_linear(config.gain_amb);
            for (int i = 0; i < num_samples; ++i) {
                const double left_main = (pca.mid_left[i] + pca.mid_right[i]) / norm_covsteps;
                const double right_main = (pca.mid_left[i] - pca.mid_right[i]) / norm_covsteps;
                const double left_amb = (pca.side_left[i] + pca.side_right[i]) / norm_covsteps;
                const double right_amb = (pca.side_left[i] - pca.side_right[i]) / norm_covsteps;
                const double gainMain = gainMainDb != nullptr
                    ? NatAmbioConstants::db_to_linear(gainMainDb[i]) : scalarGainMain;
                const double gainAmb = gainAmbDb != nullptr
                    ? NatAmbioConstants::db_to_linear(gainAmbDb[i]) : scalarGainAmb;

                left_out[i] = static_cast<float>(gainMain * left_main + gainAmb * left_amb);
                right_out[i] = static_cast<float>(gainMain * right_main + gainAmb * right_amb);
            }
        }

        for (int i = 0, j = num_samples; i < num_samples * (steps - 1); ++i, ++j) {
            pca.mid_left[i] = pca.mid_left[j];
            pca.mid_right[i] = pca.mid_right[j];
            pca.side_left[i] = pca.side_left[j];
            pca.side_right[i] = pca.side_right[j];
            pca.mid_step[i] = pca.mid_step[j];
            pca.side_step[i] = pca.side_step[j];
        }

        for (int i = num_samples * (steps - 1); i < historySamples; ++i) {
            pca.mid_left[i] = 0.0;
            pca.mid_right[i] = 0.0;
            pca.side_left[i] = 0.0;
            pca.side_right[i] = 0.0;
            pca.mid_step[i] = 0.0;
            pca.side_step[i] = 0.0;
        }

        for (int i = 0; i < steps - 1; ++i) {
            covariance.sum_xy_array[i] = covariance.sum_xy_array[i + 1];
            covariance.sum_x2_array[i] = covariance.sum_x2_array[i + 1];
            covariance.sum_y2_array[i] = covariance.sum_y2_array[i + 1];
            covariance.sum_x_array[i] = covariance.sum_x_array[i + 1];
            covariance.sum_y_array[i] = covariance.sum_y_array[i + 1];
        }

        return;
    }
}

int NaeProcessor::clampSteps(int window_length) const
{
    int steps = (window_length / std::max(1, blockSize));
    if (steps > ICORRL) steps = ICORRL;
    if (steps < 1) steps = 1;
    return steps;
}

void NaeProcessor::reconcileWindowSteps(int new_steps, int num_samples)
{
    new_steps = std::clamp(new_steps, 1, ICORRL);
    num_samples = std::max(0, std::min(num_samples, blockSize));

    if (num_samples <= 0)
        return;

    if (active_steps <= 0) {
        active_steps = new_steps;
        return;
    }

    if (new_steps == active_steps)
        return;

    if (new_steps > active_steps) {
        const int delta = new_steps - active_steps;

        // Existing slots are ordered oldest -> newest previous block, with the
        // current block about to be written at steps - 1.  When the window grows,
        // keep that chronology by moving the retained history right and exposing
        // zeros at the older edge instead of duplicating recent blocks.
        for (int src = active_steps - 2; src >= 0; --src) {
            const int dst = src + delta;
            copyHistorySlot(dst, src, num_samples);
            copyCovarianceSlot(dst, src);
        }

        for (int slot = 0; slot < delta; ++slot) {
            clearHistorySlot(slot, num_samples);
            clearCovarianceSlot(slot);
        }
        for (int slot = new_steps - 1; slot < ICORRL; ++slot) {
            clearHistorySlot(slot, num_samples);
            clearCovarianceSlot(slot);
        }
    } else {
        const int drop = active_steps - new_steps;

        // When the window shrinks, discard the oldest history and keep the most
        // recent blocks immediately before the current-block slot.
        for (int dst = 0; dst < new_steps - 1; ++dst) {
            const int src = dst + drop;
            copyHistorySlot(dst, src, num_samples);
            copyCovarianceSlot(dst, src);
        }

        for (int slot = std::max(0, new_steps - 1); slot < ICORRL; ++slot) {
            clearHistorySlot(slot, num_samples);
            clearCovarianceSlot(slot);
        }
    }

    active_steps = new_steps;
}

void NaeProcessor::clearHistorySlot(int slot, int num_samples)
{
    if (slot < 0 || slot >= ICORRL || num_samples <= 0)
        return;

    const auto offset = static_cast<size_t>(slot * num_samples);
    const auto count = static_cast<size_t>(num_samples);
    std::memset(pca.mid_step.get() + offset, 0, count * sizeof(double));
    std::memset(pca.side_step.get() + offset, 0, count * sizeof(double));
    std::memset(pca.mid_left.get() + offset, 0, count * sizeof(double));
    std::memset(pca.mid_right.get() + offset, 0, count * sizeof(double));
    std::memset(pca.side_left.get() + offset, 0, count * sizeof(double));
    std::memset(pca.side_right.get() + offset, 0, count * sizeof(double));
}

void NaeProcessor::copyHistorySlot(int dstSlot, int srcSlot, int num_samples)
{
    if (dstSlot < 0 || dstSlot >= ICORRL || srcSlot < 0 || srcSlot >= ICORRL || num_samples <= 0)
        return;

    const auto dst = static_cast<size_t>(dstSlot * num_samples);
    const auto src = static_cast<size_t>(srcSlot * num_samples);
    const auto count = static_cast<size_t>(num_samples);
    std::memcpy(pca.mid_step.get() + dst, pca.mid_step.get() + src, count * sizeof(double));
    std::memcpy(pca.side_step.get() + dst, pca.side_step.get() + src, count * sizeof(double));
    std::memcpy(pca.mid_left.get() + dst, pca.mid_left.get() + src, count * sizeof(double));
    std::memcpy(pca.mid_right.get() + dst, pca.mid_right.get() + src, count * sizeof(double));
    std::memcpy(pca.side_left.get() + dst, pca.side_left.get() + src, count * sizeof(double));
    std::memcpy(pca.side_right.get() + dst, pca.side_right.get() + src, count * sizeof(double));
}

void NaeProcessor::clearCovarianceSlot(int slot)
{
    if (slot < 0 || slot >= ICORRL)
        return;

    covariance.sum_xy_array[slot] = 0.0;
    covariance.sum_x2_array[slot] = 0.0;
    covariance.sum_y2_array[slot] = 0.0;
    covariance.sum_x_array[slot] = 0.0;
    covariance.sum_y_array[slot] = 0.0;
}

void NaeProcessor::copyCovarianceSlot(int dstSlot, int srcSlot)
{
    if (dstSlot < 0 || dstSlot >= ICORRL || srcSlot < 0 || srcSlot >= ICORRL)
        return;

    covariance.sum_xy_array[dstSlot] = covariance.sum_xy_array[srcSlot];
    covariance.sum_x2_array[dstSlot] = covariance.sum_x2_array[srcSlot];
    covariance.sum_y2_array[dstSlot] = covariance.sum_y2_array[srcSlot];
    covariance.sum_x_array[dstSlot] = covariance.sum_x_array[srcSlot];
    covariance.sum_y_array[dstSlot] = covariance.sum_y_array[srcSlot];
}

int NaeProcessor::eigen_2x2_symmetric(double a, double b, double d,
                                      double* eig1_out, double* eig2_out,
                                      double v1[2], double v2[2]) const
{
    // Use the upstream delta formulation. Unlike trace²/4-det, this retains
    // tiny off-diagonal covariance when the diagonal terms are nearly equal.
    const double trace = a + d;
    const double discriminant = std::hypot(a - d, 2.0 * b);
    const double lambda1 = 0.5 * (trace + discriminant);
    const double lambda2 = 0.5 * (trace - discriminant);

    *eig1_out = lambda1;
    *eig2_out = lambda2;

    if (std::abs(b) > 1e-12) {
        // Match the normalized upstream [1, (lambda-a)/b] formulation. Its
        // fixed first component avoids the near-zero vector that previously
        // caused a zero projector for almost-isotropic covariance matrices.
        v1[0] = 1.0;
        v1[1] = (lambda1 - a) / b;
        const double norm1 = std::hypot(v1[0], v1[1]);
        v1[0] /= norm1;
        v1[1] /= norm1;

        v2[0] = 1.0;
        v2[1] = (lambda2 - a) / b;
        const double norm2 = std::hypot(v2[0], v2[1]);
        v2[0] /= norm2;
        v2[1] /= norm2;
    } else if (a >= d) {
        v1[0] = 1.0; v1[1] = 0.0;
        v2[0] = 0.0; v2[1] = 1.0;
    } else {
        v1[0] = 0.0; v1[1] = 1.0;
        v2[0] = 1.0; v2[1] = 0.0;
    }

    return 0;
}
