/*
 * NatAmbio Constants & Structures for VST3 Plugin
 *
 * This file contains constants and data structures adapted from the upstream 
 * NatAmbio repository. All structures are derived from the original GPL v3 
 * implementation by Raul Fernandez Ortega.
 *
 * UPSTREAM REFERENCE:
 * - Repository: https://github.com/raul-fernandez-ortega/natambio
 * - Commit: 6723da8807807aacc71b5d60610dc4f6b808a120
 * - Source files: src/structs.hpp, src/nae.hpp, src/convchannel.hpp
 * 
 * ADAPTATION:
 * - These structures are simplified for VST3 stereo-only mode
 * - Removed JACK-specific port naming
 * - Removed file path handling
 * - Removed XML configuration fields
 * 
 * All audio processing algorithms remain faithful to the upstream implementation.
 *
 */

#ifndef NATAMBIO_CONSTANTS_H
#define NATAMBIO_CONSTANTS_H

#include <cmath>
#include <algorithm>

// ============================================================================
// REALTIME BUILD SWITCHES
// ============================================================================
#ifndef NATAMBIO_ENABLE_NAE
#define NATAMBIO_ENABLE_NAE 0
#endif

#ifndef NATAMBIO_ENABLE_XTC
#define NATAMBIO_ENABLE_XTC 0
#endif

#ifndef NATAMBIO_ENABLE_RT_DIAGNOSTICS
#define NATAMBIO_ENABLE_RT_DIAGNOSTICS 1
#endif

// ============================================================================
// VERIFICATION BUILD FLAG
// ============================================================================
// When NATAMBIO_VERIFICATION_BUILD=1, intermediate pipeline buffers are
// captured per-stage for comparison against upstream NatAmbio output.
//
// THIS MUST NEVER BE 1 IN RELEASE BUILDS.
// Set only via CMake for the NatAmbio_VerificationDriver target.
//
// When 0: All capture code is compiled out completely (zero cost).
// When 1: StageCapture struct is available; setCapture() enables capture.

#ifndef NATAMBIO_VERIFICATION_BUILD
#define NATAMBIO_VERIFICATION_BUILD 0
#endif

// ============================================================================
// MATH CONSTANTS
// ============================================================================

#ifndef M_PI
#define M_PI (3.14159265358979323846264338327950288)
#endif

// ============================================================================
// NAE (AMBIENT EXTRACTOR) CONSTANTS
// ============================================================================

// From upstream src/nae.hpp line 30
#define ICORRL 20  // Correlation window length for PCA

// NAE Processing Mode (from upstream struct s_nae)
enum class NaeMode : int {
    Front = 0,   // Alpha/front: main plus ambient components
    Rear = 1     // Beta/rear: correlation-panned surround component
};

// ============================================================================
// XTC (CROSSTALK CANCELLATION) CONSTANTS
// ============================================================================

// From upstream src/structs.hpp: ITD (Inter-aural Time Difference) bounds
constexpr int XTC_ITD_MIN_US = 0;           // Minimum ITD in microseconds
constexpr int XTC_ITD_MAX_US = 1000;        // Maximum ITD in microseconds
constexpr int XTC_ITD_DEFAULT_US = 500;     // Default ITD

// From upstream src/structs.hpp: ILD (Inter-aural Level Difference) bounds
constexpr double XTC_ILD_MIN_DB = 0.0;      // Minimum ILD (dB/step)
constexpr double XTC_ILD_MAX_DB = 1.0;      // Maximum ILD (dB/step)
constexpr double XTC_ILD_DEFAULT_DB = 0.5;  // Default ILD

// From upstream src/structs.hpp: Filter length constraints
constexpr int XTC_FILTER_LEN_MIN = 512;     // Minimum filter length (samples)
constexpr int XTC_FILTER_LEN_MAX = 16384;   // Maximum filter length (samples)
constexpr int XTC_FILTER_LEN_DEFAULT = 4096; // Fallback default (used when SR unknown)

// From upstream src/structs.hpp: Azimuth range (degrees)
constexpr int XTC_AZIMUTH_MIN_DEG = 0;
constexpr int XTC_AZIMUTH_MAX_DEG = 360;

// ============================================================================
// NAE CONFIGURATION
// ============================================================================

// Simplified from upstream struct s_nae for VST3 stereo mode
// Reference: upstream src/structs.hpp lines 50-65
struct NaeStereoConfig {
    // NAE Processing Mode
    NaeMode mode = NaeMode::Front;
    
    // Gain parameters (in dB)
    // From upstream s_nae.gain_main and s_nae.gain_amb
    double gain_main = 0.0;           // Main component gain (dB)
    double gain_amb = -6.0;           // Ambient component gain (dB)
    double gain_surr = 0.0;           // Rear/beta surround component gain (dB)
    
    // Processing window
    // From upstream s_nae.steps_length
    int steps_length = 2048;          // Covariance window length (samples)
    
};

// ============================================================================
// XTC CONFIGURATION
// ============================================================================

// Simplified from upstream struct xtc for VST3 stereo mode
// Reference: upstream src/structs.hpp lines 120-130
enum class XtcAlgorithm : int {
    Legacy = 0,
    Corrected = 1
};

struct XtcStereoConfig {
    // Binaural parameters
    // From upstream struct xtc
    int itd_us = XTC_ITD_DEFAULT_US;                    // Inter-aural time difference (microseconds)
    double ild_db = XTC_ILD_DEFAULT_DB;                 // Inter-aural level difference (dB/step)
    double ild_alpha = 1.0;                             // ILD model scale factor
    double ild_slope_start_hz = 200.0;                  // upstream 6 dB/oct FLIM
    
    // Spatial parameters
    int azimuth_deg = 0;                                // Source azimuth (degrees)
    
    // Filter parameters
    // From upstream struct xtc.filter_len
    int filter_len = XTC_FILTER_LEN_DEFAULT;            // Direct/cross filter length (samples)
    
    // VST3 control parameters
    float amount = 1.0f;                                // Crosstalk cancellation amount (0.0 = off, 1.0 = full)
    XtcAlgorithm algorithm = XtcAlgorithm::Corrected;
};

// ============================================================================
// COMBINED NATAMBIO STEREO CONFIGURATION
// ============================================================================

// Top-level configuration for stereo NAE + XTC processing
// This combines both NAE and XTC settings with enable/disable flags
struct NatAmbioStereoConfig {
    // Individual processors
    NaeStereoConfig nae;
    XtcStereoConfig xtc;
    
    // Enable/disable flags
    bool nae_enabled = false;         // Process through NAE?
    bool xtc_enabled = false;         // Process through XTC?

    // Route around each effect while preserving the configured latency.
    bool nae_bypassed = false;
    bool xtc_bypassed = false;
};

// === Smoothing defaults (used by NatAmbioCoreProcessor) ===
// These values prioritize "feels responsive" while eliminating clicks/zips on 24/7 and live use.
constexpr double NATAMBIO_GAIN_SMOOTH_TIME_MS = 10.0;   // NAE gains + XTC amount
constexpr int    NATAMBIO_XTC_CROSSFADE_SAMPLES = 512;  // ~10.7 ms @ 48 kHz for FIR set changes


// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// From upstream src/structs.hpp: Utility functions
namespace NatAmbioConstants {

    // Convert dB to linear scale.
    inline double db_to_linear(double db) {
        return std::pow(10.0, db / 20.0);
    }
    
    // Target time durations for FIR (in seconds). These are used to derive
    // sample counts at runtime. Because the reference ILD model does very
    // little below 200 Hz (see FLIM in XtcFilterDesigner), extremely long
    // FIRs for sub-bass are not required for reference behavior.
    constexpr double XTC_TARGET_FULL_DURATION_SEC = 4096.0 / 48000.0;   // ~85 ms full filter (matches 4096@48k)

    // === Recommended XTC FIR lengths (runtime policy for quality/performance) ===
    // Computes a filter length in samples that targets a roughly constant time
    // duration across sample rates. This keeps similar "acoustic length" of the
    // crosstalk cancellation model.
    // Because the reference model (FLIM=200 Hz) intentionally does little below
    // 200 Hz, we do not need extreme lengths purely for sub-bass.
    inline int recommendedXtcFilterLen(int sampleRate, double targetSeconds = XTC_TARGET_FULL_DURATION_SEC)
    {
        if (sampleRate <= 0)
            return XTC_FILTER_LEN_DEFAULT;
        const double taps = targetSeconds * static_cast<double>(sampleRate);
        int len = static_cast<int>(std::lround(taps));
        len = (std::max)(XTC_FILTER_LEN_MIN, len);
        len = (std::min)(XTC_FILTER_LEN_MAX, len);
        // Preserve the duration target while choosing an odd exact length.
        // The odd-length Bluestein path matches upstream FFTW measurably better
        // (4097@48k, 7527@88.2k, 8193@96k).
        if (len < XTC_FILTER_LEN_MAX && (len & 1) == 0)
            ++len;
        return len;
    }
}

#endif  // NATAMBIO_CONSTANTS_H
