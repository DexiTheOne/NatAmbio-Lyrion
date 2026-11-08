#ifndef XTC_FILTER_DESIGNER_H
#define XTC_FILTER_DESIGNER_H

#include "NatAmbioConstants.h"
#include <vector>

struct XtcDesignedFilters {
    std::vector<float> direct;
    std::vector<float> cross;
    int sampleRate = 0;
    int itdSamples = 0;
    int latencySamples = 0;
};

class XtcFilterDesigner {
public:
    static XtcDesignedFilters design(const XtcStereoConfig& config, int sampleRate);
    /** Read-only UI/test view of the exact ILD curve used by FIR design. */
    static double evaluateIldDb(double frequencyHz, double azimuthDegrees,
                                double alpha, double slopeStartHz);
    // Exposed for the experimental completed-matrix filter realisations and
    // their focused tests; faithful design still enters through design().
    static std::vector<double> minimumPhase(
        const std::vector<double>& input,
        XtcAlgorithm algorithm = XtcAlgorithm::Corrected);
    static std::vector<double> fftConvolveTruncate(const std::vector<double>& a,
                                                   const std::vector<double>& b,
                                                   int outputLength,
                                                   bool exactTwiddles = false);

private:
    static double degToRad(double degrees);
    static double ildLogEmpirical(double theta, double frequencyHz, double alpha,
                                  double slopeStartHz);
    static double ildDbModel(double frequencyHz, double theta, double alpha,
                             double slopeStartHz);
    static std::vector<double> firwin2(int numTaps, int sampleRate, double theta,
                                       double alpha, double slopeStartHz);
    static void getXtc(int length,
                       double attenuationDb,
                       int delaySamples,
                       const std::vector<double>& ildFilter,
                       std::vector<double>& direct,
                       std::vector<double>& cross,
                       XtcAlgorithm algorithm);
};

#endif
