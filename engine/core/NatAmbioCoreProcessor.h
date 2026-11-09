/*
 * NatAmbio Core Processor
 *
 * JUCE-free stereo NAE + XTC orchestration shared by plugin wrappers.
 *
 * Upstream reference:
 * - https://github.com/raul-fernandez-ortega/natambio
 * - Commit: 6723da8807807aacc71b5d60610dc4f6b808a120
 */

#ifndef NATAMBIO_CORE_PROCESSOR_H
#define NATAMBIO_CORE_PROCESSOR_H

#include "NatAmbioConstants.h"
#include "NaeProcessor.h"
#include "XtcProcessor.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#if NATAMBIO_VERIFICATION_BUILD
struct StageCapture {
    std::vector<float> input_L, input_R;
    std::vector<float> nae_out_L, nae_out_R;
    std::vector<float> xtc_in_L, xtc_in_R;
    std::vector<float> xtc_out_L, xtc_out_R;
    std::vector<float> final_L, final_R;

    bool nae_was_active = false;
    bool xtc_was_active = false;

    void clear()
    {
        input_L.clear();
        input_R.clear();
        nae_out_L.clear();
        nae_out_R.clear();
        xtc_in_L.clear();
        xtc_in_R.clear();
        xtc_out_L.clear();
        xtc_out_R.clear();
        final_L.clear();
        final_R.clear();
        nae_was_active = false;
        xtc_was_active = false;
    }

    static void appendBlock(const float* src_L, const float* src_R, int n,
                            std::vector<float>& dst_L, std::vector<float>& dst_R)
    {
        dst_L.insert(dst_L.end(), src_L, src_L + n);
        dst_R.insert(dst_R.end(), src_R, src_R + n);
    }
};
#endif

class NatAmbioCoreProcessor {
public:
    NatAmbioCoreProcessor(int sampleRate, int blockSize);
    ~NatAmbioCoreProcessor();

    void prepare(int sampleRate, int blockSize);
    void configureXtc(const XtcStereoConfig& config);
    void setXtcProcessor(std::unique_ptr<XtcProcessor> processor);

    void process(const float* left_in, const float* right_in,
                 float* left_out, float* right_out,
                 int numSamples,
                 const NatAmbioStereoConfig& config);

    void processWithNaeTap(const float* left_in, const float* right_in,
                           float* left_out, float* right_out,
                           float* nae_tap_left, float* nae_tap_right,
                           int numSamples,
                           const NatAmbioStereoConfig& config);

    void reset();

    int getLatencySamples(const NatAmbioStereoConfig& config) const;
    NaeProcessor::PcaAnalysis getNaePcaAnalysis() const noexcept;
    void setPcaReporting(bool enabled) noexcept;

#if NATAMBIO_VERIFICATION_BUILD
    void setCapture(StageCapture* target) noexcept { capture_target_ = target; }
#endif

private:
    static float sanitizeInputSample(float value) noexcept;
    static float sanitizeOutputSample(float value) noexcept;
    int clampNaeSteps(int stepsLength) const noexcept;

    void sanitizeInputBlock(const float* left_in, const float* right_in, int numSamples) noexcept;
    void sanitizeOutputBlock(float* left_out, float* right_out, int numSamples) noexcept;

    void processBufferedBlocks(const float* left_in, const float* right_in,
                               float* left_out, float* right_out,
                               float* nae_tap_left, float* nae_tap_right,
                               int numSamples,
                               const NatAmbioStereoConfig& config);

    void processFixedBlock(const float* left_in, const float* right_in,
                           float* left_out, float* right_out,
                           float* nae_tap_left, float* nae_tap_right,
                           const NatAmbioStereoConfig& config,
                           int numSamples);

    void fillNaeGainRamps(int numSamples) noexcept;
    float nextSmoothedXtcAmount() noexcept;
    float advanceBypassMix(float mix, bool bypassed) const noexcept;
    void blendNaeBypass(const NatAmbioStereoConfig& config, int numSamples) noexcept;

    void beginPendingXtcWarm();
    void appendWarmingXtcInput(const float* left, const float* right, int numSamples) noexcept;
    void advanceWarmingXtc(const XtcStereoConfig& config, int numSamples, bool allowInstall);
    void installWarmedXtcOwner() noexcept;
    void retireFinishedXtcOwner() noexcept;
    void reclaimRetiredXtcOwner() noexcept;
    void discardPendingXtcCommand() noexcept;
    void clearXtcOwnershipOnStoppedThread() noexcept;
    void warmXtcProcessor(XtcProcessor& processor, const XtcStereoConfig& config);
    void appendXtcHistory(const float* left, const float* right, int numSamples) noexcept;

    void resetBlockAdapter() noexcept;
    void pushOutputSamples(const float* left, const float* right,
                           const float* nae_tap_left, const float* nae_tap_right,
                           int numSamples) noexcept;
    int popOutputSamples(float* left, float* right,
                         float* nae_tap_left, float* nae_tap_right,
                         int numSamples) noexcept;

    std::unique_ptr<NaeProcessor> nae_processor;
    std::unique_ptr<NaeProcessor> fading_nae_processor;

    // setXtcProcessor() publishes ownership from a non-audio producer. One
    // tagged atomic word is the complete command: 0 is empty, 1 disables XTC,
    // and every aligned pointer transfers a prepared processor. This makes
    // coalescing linearizable without a generation/pointer race.
    //
    // The audio thread owns active/warming/fading exclusively. Completed fades
    // are returned through a one-slot raw-pointer mailbox; if it is occupied,
    // fading ownership is retained as backpressure. Consequently the audio
    // thread never destroys a processor or touches shared reference counts.
    std::unique_ptr<XtcProcessor> active_xtc_owner;
    std::unique_ptr<XtcProcessor> fading_xtc_owner;
    std::unique_ptr<XtcProcessor> warming_xtc_owner;
    std::atomic<std::uintptr_t> pending_xtc_command { 0 };
    std::atomic<XtcProcessor*> retired_xtc_owner { nullptr };

    std::vector<float> sanitized_input_left;
    std::vector<float> sanitized_input_right;
    std::vector<float> nae_left_out;
    std::vector<float> nae_right_out;
    std::vector<float> naeDryHistoryLeft;
    std::vector<float> naeDryHistoryRight;
    int naeDryWriteFrame = 0;
    float naeBypassMix = 0.0f;
    float xtcBypassMix = 0.0f;
    float bypassFadeStep = 1.0f;
    bool bypassMixInitialized = false;

    std::vector<float> buffered_input_left;
    std::vector<float> buffered_input_right;
    std::vector<float> buffered_output_left;
    std::vector<float> buffered_output_right;
    std::vector<float> buffered_nae_tap_left;
    std::vector<float> buffered_nae_tap_right;
    std::vector<float> output_fifo_left;
    std::vector<float> output_fifo_right;
    std::vector<float> nae_tap_fifo_left;
    std::vector<float> nae_tap_fifo_right;
    std::vector<float> naeFadeOldLeft;
    std::vector<float> naeFadeOldRight;
    std::vector<double> naeGainMainRamp;
    std::vector<double> naeGainAmbRamp;

    int inputFrameFill = 0;
    int outputFifoReadPosition = 0;
    int outputFifoWritePosition = 0;
    int outputFifoSamples = 0;
    bool fixedFrameAdapterActive = false;

    int sampleRate = 48000;
    int blockSize = 256;

    // === Parameter smoothing for click-free automation and live tweaks ===
    // One-pole smoothing on gains (NAE) and amount (XTC). Time constant ~8-12 ms.
    // Targets taken from incoming config each block; smoothed values fed to sub-processors.
    double targetNaeGainMain = 0.0;
    double targetNaeGainAmb = 0.0;
    float targetXtcAmount = 1.0f;

    double smoothedNaeGainMain = 0.0;
    double smoothedNaeGainAmb = 0.0;
    float smoothedXtcAmount = 1.0f;
    double gainSmoothRetentionPerSample = 0.0; // exp(-1/tau), computed in prepare()

    // === NAE window crossfade ===
    // Changing the covariance window alters PCA history and can otherwise
    // expose a short discontinuity. A second pre-allocated NAE instance keeps
    // the old window running briefly so the core can fade into the new one.
    NaeStereoConfig lastEffectiveNaeConfig {};
    NaeStereoConfig fadingNaeConfig {};
    bool lastEffectiveNaeConfigValid = false;
    bool fadingNaeActive = false;
    int naeCrossfadeRemaining = 0;
    int naeCrossfadeLength = 2048;

    // === XTC processor crossfade on filter set changes ===
    int xtcCrossfadeRemaining = 0;
    int xtcCrossfadeLength = 512;

    std::vector<float> xtcFadeOldLeft;
    std::vector<float> xtcFadeOldRight;
    std::vector<float> xtcHistoryLeft;
    std::vector<float> xtcHistoryRight;
    std::vector<float> xtcWarmInputLeft;
    std::vector<float> xtcWarmInputRight;
    std::vector<float> xtcWarmOutputLeft;
    std::vector<float> xtcWarmOutputRight;
    std::vector<float> xtcWarmBacklogLeft;
    std::vector<float> xtcWarmBacklogRight;

    int xtcHistoryWritePosition = 0;
    int xtcHistorySamples = 0;
    int xtcHistoryStreamPhase = 0;
    int xtcWarmBacklogReadPosition = 0;
    int xtcWarmBacklogWritePosition = 0;
    int xtcWarmBacklogSamples = 0;
    bool lastXtcProcessingEnabled = false;

#if NATAMBIO_VERIFICATION_BUILD
    StageCapture* capture_target_ = nullptr;
#endif
};

#endif
