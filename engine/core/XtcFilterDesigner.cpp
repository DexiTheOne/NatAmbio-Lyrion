#include "XtcFilterDesigner.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

namespace {
constexpr double kDbOct = 6.0;
constexpr double kHighShelfHz = 20000.0;
constexpr double kHighShelfDbOct = -36.0;
constexpr int kNumSteps = 32;
constexpr double kLogEps = 1.0e-12;

int ceilLog2Pow(int value)
{
    int power = 1;
    while (power < value)
        power <<= 1;
    return power;
}

bool isPowerOfTwo(int value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

double rms(const std::vector<double>& data)
{
    if (data.empty())
        return 0.0;

    double sum = 0.0;
    for (double v : data)
        sum += v * v;
    return std::sqrt(sum / static_cast<double>(data.size()));
}

double l2Norm(const std::vector<double>& data)
{
    double sum = 0.0;
    for (double v : data)
        sum += v * v;
    return std::sqrt(sum);
}

void normalizeBy(std::vector<double>& data, double divisor)
{
    if (divisor <= 0.0)
        return;

    for (double& v : data)
        v /= divisor;
}

void radix2Fft(std::vector<std::complex<double>>& a, bool inverse, bool exactTwiddles = false)
{
    const int n = static_cast<int>(a.size());
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[static_cast<size_t>(i)], a[static_cast<size_t>(j)]);
    }

    for (int len = 2; len <= n; len <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * M_PI / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(angle), std::sin(angle));
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (int j = 0; j < len / 2; ++j) {
                if (exactTwiddles)
                    w = { std::cos(angle * j), std::sin(angle * j) };
                const auto u = a[static_cast<size_t>(i + j)];
                const auto v = a[static_cast<size_t>(i + j + len / 2)] * w;
                a[static_cast<size_t>(i + j)] = u + v;
                a[static_cast<size_t>(i + j + len / 2)] = u - v;
                if (!exactTwiddles)
                    w *= wlen;
            }
        }
    }

    if (inverse) {
        const double scale = 1.0 / static_cast<double>(n);
        for (auto& v : a)
            v *= scale;
    }
}

// The upstream minimum-phase transform asks FFTW to transform exactly n
// samples. Bluestein's algorithm preserves that DFT length while allowing us
// to keep the small, dependency-free radix-2 FFT used elsewhere in this port.
void fft(std::vector<std::complex<double>>& a, bool inverse, bool exactTwiddles = false)
{
    const int n = static_cast<int>(a.size());
    if (n <= 1)
        return;

    if (isPowerOfTwo(n)) {
        radix2Fft(a, inverse, exactTwiddles);
        return;
    }

    const int convolutionSize = ceilLog2Pow(2 * n - 1);
    std::vector<std::complex<double>> chirped(static_cast<size_t>(convolutionSize));
    std::vector<std::complex<double>> kernel(static_cast<size_t>(convolutionSize));
    const double direction = inverse ? 1.0 : -1.0;

    for (int i = 0; i < n; ++i) {
        const auto phaseIndex = static_cast<long long>(i) * static_cast<long long>(i)
            % static_cast<long long>(2 * n);
        const double angle = direction * M_PI * static_cast<double>(phaseIndex)
            / static_cast<double>(n);
        const std::complex<double> chirp(std::cos(angle), std::sin(angle));
        const std::complex<double> kernelValue(std::cos(-angle), std::sin(-angle));
        chirped[static_cast<size_t>(i)] = a[static_cast<size_t>(i)] * chirp;
        kernel[static_cast<size_t>(i)] = kernelValue;
        if (i != 0)
            kernel[static_cast<size_t>(convolutionSize - i)] = kernelValue;
    }

    radix2Fft(chirped, false, true);
    radix2Fft(kernel, false, true);
    for (int i = 0; i < convolutionSize; ++i)
        chirped[static_cast<size_t>(i)] *= kernel[static_cast<size_t>(i)];
    radix2Fft(chirped, true, true);

    const double scale = inverse ? 1.0 / static_cast<double>(n) : 1.0;
    for (int i = 0; i < n; ++i) {
        const auto phaseIndex = static_cast<long long>(i) * static_cast<long long>(i)
            % static_cast<long long>(2 * n);
        const double angle = direction * M_PI * static_cast<double>(phaseIndex)
            / static_cast<double>(n);
        const std::complex<double> chirp(std::cos(angle), std::sin(angle));
        a[static_cast<size_t>(i)] = chirped[static_cast<size_t>(i)] * chirp * scale;
    }
}

std::vector<std::complex<double>> rfft(const std::vector<double>& input,
                                      int fftSize,
                                      bool exactTwiddles = false)
{
    std::vector<std::complex<double>> data(static_cast<size_t>(fftSize));
    const int copyCount = std::min(static_cast<int>(input.size()), fftSize);
    for (int i = 0; i < copyCount; ++i)
        data[static_cast<size_t>(i)] = input[static_cast<size_t>(i)];
    fft(data, false, exactTwiddles);
    data.resize(static_cast<size_t>(fftSize / 2 + 1));
    return data;
}

std::vector<double> irfft(const std::vector<std::complex<double>>& halfSpectrum,
                          int fftSize,
                          bool exactTwiddles = false)
{
    std::vector<std::complex<double>> spectrum(static_cast<size_t>(fftSize));
    const int halfSize = static_cast<int>(halfSpectrum.size());
    for (int i = 0; i < halfSize; ++i)
        spectrum[static_cast<size_t>(i)] = halfSpectrum[static_cast<size_t>(i)];
    for (int i = 1; i <= (fftSize - 1) / 2; ++i)
        spectrum[static_cast<size_t>(fftSize - i)] = std::conj(spectrum[static_cast<size_t>(i)]);

    fft(spectrum, true, exactTwiddles);

    std::vector<double> out(static_cast<size_t>(fftSize), 0.0);
    for (int i = 0; i < fftSize; ++i)
        out[static_cast<size_t>(i)] = spectrum[static_cast<size_t>(i)].real();
    return out;
}

}

XtcDesignedFilters XtcFilterDesigner::design(const XtcStereoConfig& config, int sampleRate)
{
    XtcDesignedFilters result;
    result.sampleRate = sampleRate;

    const int filterLen = std::max(2, config.filter_len);
    result.itdSamples = static_cast<int>(std::llround(static_cast<double>(config.itd_us) * 1.0e-6
                                                      * static_cast<double>(sampleRate)));
    if (result.itdSamples <= 0)
        result.itdSamples = 1;

    const double theta = degToRad(static_cast<double>(config.azimuth_deg));

    std::vector<double> hLinear = firwin2(filterLen, sampleRate, theta,
                                          config.ild_alpha,
                                          config.ild_slope_start_hz);
    normalizeBy(hLinear, rms(hLinear));

    std::vector<double> hMin = minimumPhase(hLinear, config.algorithm);
    normalizeBy(hMin, rms(hMin));
    normalizeBy(hMin, l2Norm(hMin));

    std::vector<double> direct;
    std::vector<double> cross;
    getXtc(filterLen, config.ild_db, result.itdSamples, hMin, direct, cross,
           config.algorithm);

    const int outputLength = static_cast<int>(direct.size());
    result.direct.resize(static_cast<size_t>(outputLength));
    result.cross.resize(static_cast<size_t>(outputLength));
    for (int i = 0; i < outputLength; ++i) {
        result.direct[static_cast<size_t>(i)] = static_cast<float>(direct[static_cast<size_t>(i)]);
        result.cross[static_cast<size_t>(i)] = static_cast<float>(cross[static_cast<size_t>(i)]);
    }
    return result;
}

double XtcFilterDesigner::degToRad(double degrees)
{
    return degrees * M_PI / 180.0;
}

double XtcFilterDesigner::evaluateIldDb(double frequencyHz,
                                        double azimuthDegrees,
                                        double alpha,
                                        double slopeStartHz)
{
    if (!std::isfinite(frequencyHz) || !std::isfinite(azimuthDegrees)
        || !std::isfinite(alpha) || !std::isfinite(slopeStartHz)
        || frequencyHz <= 0.0)
        return 0.0;
    return ildDbModel(frequencyHz, degToRad(azimuthDegrees), alpha, slopeStartHz);
}

double XtcFilterDesigner::ildLogEmpirical(double theta, double frequencyHz, double alpha,
                                          double slopeStartHz)
{
    const double f = std::max(frequencyHz, slopeStartHz);
    const double aF = alpha * 10.0 * std::log10(f / 1000.0 + 1.0);
    return aF * std::sin(theta);
}

double XtcFilterDesigner::ildDbModel(double frequencyHz, double theta, double alpha,
                                     double requestedSlopeStartHz)
{
    const double slopeStartHz = std::clamp(requestedSlopeStartHz, 60.0, 500.0);
    double db = 0.0;
    if (frequencyHz > slopeStartHz) {
        db = -ildLogEmpirical(theta, frequencyHz, alpha, slopeStartHz);
    } else {
        const double ildFLim = ildLogEmpirical(theta, slopeStartHz, alpha,
                                               slopeStartHz);
        db = -kDbOct * (std::log2(slopeStartHz) - std::log2(frequencyHz)) - ildFLim;
    }

    if (frequencyHz > kHighShelfHz)
        db += kHighShelfDbOct * std::log2(frequencyHz / kHighShelfHz);

    return db;
}

std::vector<double> XtcFilterDesigner::firwin2(int numTaps, int sampleRate, double theta,
                                               double alpha, double slopeStartHz)
{
    const int nfreqs = 1 + ceilLog2Pow(numTaps);
    const int realSize = 2 * (nfreqs - 1);
    const double nyquistHz = 0.5 * static_cast<double>(sampleRate);
    const double delay = 0.5 * static_cast<double>(numTaps - 1);

    std::vector<std::complex<double>> spectrum(static_cast<size_t>(nfreqs));
    for (int i = 0; i < nfreqs; ++i) {
        double gainLinear = 0.0;
        if (i != 0 && i != nfreqs - 1) {
            const double frequencyHz = (static_cast<double>(i) / static_cast<double>(nfreqs - 1)) * nyquistHz;
            gainLinear = std::pow(10.0,
                ildDbModel(frequencyHz, theta, alpha, slopeStartHz) / 20.0);
        }

        const double x = static_cast<double>(i) / static_cast<double>(nfreqs - 1);
        const double phase = -M_PI * x * delay;
        spectrum[static_cast<size_t>(i)] = gainLinear * std::complex<double>(std::cos(phase), std::sin(phase));
    }

    std::vector<double> time = irfft(spectrum, realSize);
    std::vector<double> out(static_cast<size_t>(numTaps), 0.0);
    for (int n = 0; n < numTaps; ++n) {
        const double window = 0.54 - 0.46 * std::cos(2.0 * M_PI * static_cast<double>(n)
                                                      / static_cast<double>(numTaps - 1));
        out[static_cast<size_t>(n)] = time[static_cast<size_t>(n)] * window;
    }
    return out;
}

std::vector<double> XtcFilterDesigner::minimumPhase(
    const std::vector<double>& input,
    XtcAlgorithm algorithm)
{
    const int n = static_cast<int>(input.size());
    const int fftSize = algorithm == XtcAlgorithm::Corrected
                      ? ceilLog2Pow(n) * 8
                      : n;
    const bool corrected = algorithm == XtcAlgorithm::Corrected;
    std::vector<std::complex<double>> xFreq = rfft(input, fftSize, corrected);

    std::vector<std::complex<double>> logMag(static_cast<size_t>(fftSize / 2 + 1));
    for (int i = 0; i <= fftSize / 2; ++i)
        logMag[static_cast<size_t>(i)] = std::log(std::abs(xFreq[static_cast<size_t>(i)]) + kLogEps);

    std::vector<double> ceps = irfft(logMag, fftSize, corrected);

    if ((fftSize % 2) == 0) {
        for (int i = 1; i < fftSize / 2; ++i)
            ceps[static_cast<size_t>(i)] *= 2.0;
        for (int i = fftSize / 2 + 1; i < fftSize; ++i)
            ceps[static_cast<size_t>(i)] = 0.0;
    } else {
        for (int i = 1; i <= (fftSize - 1) / 2; ++i)
            ceps[static_cast<size_t>(i)] *= 2.0;
        for (int i = (fftSize - 1) / 2 + 1; i < fftSize; ++i)
            ceps[static_cast<size_t>(i)] = 0.0;
    }

    std::vector<std::complex<double>> cepsFreq = rfft(ceps, fftSize, corrected);
    for (auto& v : cepsFreq)
        v = std::exp(v);

    auto minimum = irfft(cepsFreq, fftSize, corrected);
    minimum.resize(static_cast<size_t>(n));
    return minimum;
}

std::vector<double> XtcFilterDesigner::fftConvolveTruncate(const std::vector<double>& a,
                                                           const std::vector<double>& b,
                                                           int outputLength,
                                                           bool exactTwiddles)
{
    const int convLength = static_cast<int>(a.size() + b.size() - 1);
    const int fftSize = ceilLog2Pow(convLength);

    std::vector<std::complex<double>> af(static_cast<size_t>(fftSize));
    std::vector<std::complex<double>> bf(static_cast<size_t>(fftSize));

    for (size_t i = 0; i < a.size(); ++i)
        af[i] = a[i];
    for (size_t i = 0; i < b.size(); ++i)
        bf[i] = b[i];

    fft(af, false, exactTwiddles);
    fft(bf, false, exactTwiddles);
    for (int i = 0; i < fftSize; ++i)
        af[static_cast<size_t>(i)] *= bf[static_cast<size_t>(i)];
    fft(af, true, exactTwiddles);

    std::vector<double> out(static_cast<size_t>(outputLength), 0.0);
    const int copyCount = std::min(outputLength, convLength);
    for (int i = 0; i < copyCount; ++i)
        out[static_cast<size_t>(i)] = af[static_cast<size_t>(i)].real();
    return out;
}

void XtcFilterDesigner::getXtc(int length,
                               double attenuationDb,
                               int delaySamples,
                               const std::vector<double>& ildFilter,
                               std::vector<double>& direct,
                               std::vector<double>& cross,
                               XtcAlgorithm algorithm)
{
    direct.assign(static_cast<size_t>(length), 0.0);
    cross.assign(static_cast<size_t>(length), 0.0);

    int n = (kNumSteps - 1) * delaySamples;
    double crossAtt = -attenuationDb * static_cast<double>(kNumSteps - 1);
    double dirAtt = -attenuationDb * static_cast<double>(kNumSteps);

    while (n > 0) {
        if (n + delaySamples < length)
            direct[static_cast<size_t>(n + delaySamples)] += std::pow(10.0, dirAtt / 20.0);

        if (n < length)
            cross[static_cast<size_t>(n)] += -std::pow(10.0, crossAtt / 20.0);

        const bool corrected = algorithm == XtcAlgorithm::Corrected;
        direct = fftConvolveTruncate(direct, ildFilter, length, corrected);
        cross = fftConvolveTruncate(cross, ildFilter, length, corrected);

        dirAtt += 2.0 * attenuationDb;
        crossAtt += 2.0 * attenuationDb;
        n -= 2 * delaySamples;
    }

    direct[0] = 1.0;
}
