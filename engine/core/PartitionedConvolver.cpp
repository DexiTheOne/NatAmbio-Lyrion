#include "PartitionedConvolver.h"

#include <algorithm>
#include <cstring>

namespace NatAmbio {

PartitionedConvolver::PartitionedConvolver() = default;

void PartitionedConvolver::fft(std::vector<std::complex<float>>& a, bool inverse) {
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
        const float angle = (inverse ? 2.0f : -2.0f) * 3.14159265358979323846f / static_cast<float>(len);
        const std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; ++j) {
                const auto u = a[static_cast<size_t>(i + j)];
                const auto v = a[static_cast<size_t>(i + j + len / 2)] * w;
                a[static_cast<size_t>(i + j)] = u + v;
                a[static_cast<size_t>(i + j + len / 2)] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse) {
        const float scale = 1.0f / static_cast<float>(n);
        for (auto& v : a)
            v *= scale;
    }
}

void PartitionedConvolver::prepare(const float* filter, int filterLength, int partSize, int /*sampleRate*/) {
    if (filterLength <= 0 || partSize <= 0) {
        prepared = false;
        return;
    }
    partitionSize = partSize;
    filterLen = filterLength;
    partitionCount = (filterLen + partitionSize - 1) / partitionSize;

    // Uniform partitioned convolution: each filter/input partition is partSize
    // samples, zero-padded to a power-of-two FFT that can hold 2*partSize - 1.
    int needed = std::max(1, 2 * partitionSize);
    fftSize = 1;
    while (fftSize < needed) fftSize <<= 1;

    filterPartitions.assign(static_cast<size_t>(partitionCount * fftSize), {});
    inputSpectra.assign(static_cast<size_t>(partitionCount * fftSize), {});
    accumSpectrum.assign(static_cast<size_t>(fftSize), {});
    fftWork.assign(static_cast<size_t>(fftSize), {});

    for (int partition = 0; partition < partitionCount; ++partition) {
        std::fill(fftWork.begin(), fftWork.end(), std::complex<float> {});
        const int filterOffset = partition * partitionSize;
        const int samplesThisPartition = std::min(partitionSize, filterLength - filterOffset);
        for (int i = 0; i < samplesThisPartition; ++i) {
            fftWork[static_cast<size_t>(i)] = filter[filterOffset + i];
        }
        fft(fftWork, false);

        auto* partitionFft = filterPartitions.data() + static_cast<size_t>(partition * fftSize);
        std::copy(fftWork.begin(), fftWork.end(), partitionFft);
    }

    overlap.assign(static_cast<size_t>(fftSize), 0.0f);
    pendingInput.assign(static_cast<size_t>(partitionSize), 0.0f);
    writePos = 0;
    pendingCount = 0;
    latencySamples = 0;
    prepared = true;
}

void PartitionedConvolver::process(const float* input, float* output, int numSamples) {
    if (!prepared || numSamples <= 0) {
        if (numSamples > 0) std::memcpy(output, input, numSamples * sizeof(float));
        return;
    }

    int offset = 0;
    while (offset < numSamples) {
        const int previousPending = pendingCount;
        const int chunk = std::min(partitionSize - pendingCount, numSamples - offset);
        std::copy_n(input + offset,
                    chunk,
                    pendingInput.data() + static_cast<size_t>(pendingCount));
        pendingCount += chunk;
        processPendingInput(output + offset, previousPending, chunk);
        offset += chunk;
    }
}

void PartitionedConvolver::processPendingInput(float* output, int outputStart, int numOutput) {
    std::fill(fftWork.begin(), fftWork.end(), std::complex<float> {});
    for (int i = 0; i < pendingCount; ++i) {
        fftWork[static_cast<size_t>(i)] = pendingInput[static_cast<size_t>(i)];
    }
    fft(fftWork, false);

    convolveSpectrum(fftWork.data(), writePos);

    for (int i = 0; i < numOutput; ++i) {
        const int partitionIndex = outputStart + i;
        output[i] = accumSpectrum[static_cast<size_t>(partitionIndex)].real()
            + overlap[static_cast<size_t>(partitionIndex)];
    }

    if (pendingCount != partitionSize)
        return;

    auto* currentInputSpectrum = inputSpectra.data() + static_cast<size_t>(writePos * fftSize);
    std::copy(fftWork.begin(), fftWork.end(), currentInputSpectrum);

    updateOverlap();

    writePos = (writePos + 1) % partitionCount;
    pendingCount = 0;
}

void PartitionedConvolver::convolveSpectrum(const std::complex<float>* currentSpectrum,
                                             int currentPosition) {
    const auto* firstFilterFft = filterPartitions.data();
    for (int bin = 0; bin < fftSize; ++bin) {
        accumSpectrum[static_cast<size_t>(bin)] =
            currentSpectrum[bin] * firstFilterFft[static_cast<size_t>(bin)];
    }

    for (int partition = 1; partition < partitionCount; ++partition) {
        int inputIndex = currentPosition - partition;
        if (inputIndex < 0) {
            inputIndex += partitionCount;
        }

        const auto* inputFft = inputSpectra.data() + static_cast<size_t>(inputIndex * fftSize);
        const auto* filterFft = filterPartitions.data() + static_cast<size_t>(partition * fftSize);
        for (int bin = 0; bin < fftSize; ++bin) {
            accumSpectrum[static_cast<size_t>(bin)] +=
                inputFft[static_cast<size_t>(bin)] * filterFft[static_cast<size_t>(bin)];
        }
    }

    fft(accumSpectrum, true);
}

void PartitionedConvolver::updateOverlap() {
    std::fill(overlap.begin(), overlap.end(), 0.0f);
    for (int i = partitionSize; i < fftSize; ++i) {
        overlap[static_cast<size_t>(i - partitionSize)] = accumSpectrum[static_cast<size_t>(i)].real();
    }
}

bool PartitionedConvolver::copyInputHistoryFrom(const PartitionedConvolver& source) noexcept {
    if (!prepared || !source.prepared
        || partitionSize != source.partitionSize || partitionCount != source.partitionCount)
        return false;

    writePos = source.writePos;
    pendingCount = source.pendingCount;
    std::copy(source.inputSpectra.begin(), source.inputSpectra.end(), inputSpectra.begin());
    std::copy_n(source.pendingInput.data(), pendingCount, pendingInput.data());

    // The last completed frame contributes an overlap computed with the new
    // filter. Its input spectrum and the older partitions are already cached.
    const int previousPosition = (writePos + partitionCount - 1) % partitionCount;
    const auto* previousSpectrum = inputSpectra.data() + static_cast<size_t>(previousPosition) * fftSize;
    convolveSpectrum(previousSpectrum, previousPosition);
    updateOverlap();
    return true;
}

void PartitionedConvolver::reset() {
    writePos = 0;
    pendingCount = 0;
    std::fill(inputSpectra.begin(), inputSpectra.end(), std::complex<float> {});
    std::fill(accumSpectrum.begin(), accumSpectrum.end(), std::complex<float> {});
    std::fill(overlap.begin(), overlap.end(), 0.0f);
    std::fill(pendingInput.begin(), pendingInput.end(), 0.0f);
    std::fill(fftWork.begin(), fftWork.end(), std::complex<float> {});
}

} // namespace NatAmbio
