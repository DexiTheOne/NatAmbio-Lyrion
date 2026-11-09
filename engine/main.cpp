#include "NatAmbioCoreProcessor.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr int blockSize = 256;
constexpr int timelineSlots = 30 * 180;
constexpr int timelineRecordBytes = 512;
constexpr auto viewerCheckInterval = std::chrono::milliseconds(200);
using Clock = std::chrono::steady_clock;
using Stereo = std::array<float, 2>;

double threadCpuSeconds()
{
    timespec time{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
    return time.tv_sec + time.tv_nsec * 1e-9;
}

std::uint32_t readWord(std::istream& stream, int bytes = 4)
{
    std::uint32_t word = 0;
    for (int i = 0; i < bytes; ++i) {
        const int byte = stream.get();
        if (byte == EOF)
            throw std::runtime_error("truncated AU data");
        word = (word << 8) | static_cast<std::uint32_t>(byte);
    }
    return word;
}

void writeWord(std::ostream& stream, std::uint32_t word, int bytes = 4)
{
    for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8)
        stream.put(static_cast<char>((word >> shift) & 255));
}

struct Au {
    int rate;
    int bytes;
    int channels;
    std::uint64_t frames;
    bool indefinite;
};

Au readHeader(std::istream& stream)
{
    if (readWord(stream) != 0x2e736e64)
        throw std::runtime_error("expected Sun AU input");
    const auto offset = readWord(stream);
    const auto length = readWord(stream);
    const auto encoding = readWord(stream);
    const auto rate = readWord(stream);
    const auto channels = readWord(stream);
    if (offset < 24 || offset > 65536 || encoding < 3 || encoding > 5
        || rate < 8000 || rate > 192000 || channels < 1 || channels > 2)
        throw std::runtime_error("unsupported AU input format");
    stream.ignore(offset - 24);
    const auto frameBytes = (encoding - 1) * channels;
    return {static_cast<int>(rate), static_cast<int>(encoding - 1),
            static_cast<int>(channels), length / frameBytes, length == 0xffffffffu};
}

float readSample(std::istream& stream, int bytes)
{
    const auto word = readWord(stream, bytes);
    const auto scale = std::int64_t{1} << (bytes * 8 - 1);
    const auto signedValue = word >= scale ? word - 2 * scale : word;
    return static_cast<float>(static_cast<double>(signedValue) / scale);
}

void writeHeader(const Au& au)
{
    writeWord(std::cout, 0x2e736e64);
    writeWord(std::cout, 32);
    const auto size = au.frames * 6;
    writeWord(std::cout, au.indefinite || size >= 0xffffffffu ? 0xffffffffu : size);
    writeWord(std::cout, 4);
    writeWord(std::cout, au.rate);
    writeWord(std::cout, 2);
    writeWord(std::cout, 0);
    writeWord(std::cout, 0);
}

class TrackLock {
public:
    explicit TrackLock(const std::string& path)
        : fd(open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600))
    {
        if (fd < 0)
            throw std::runtime_error("cannot open player lock");
        int result;
        do {
            result = flock(fd, LOCK_EX);
        } while (result != 0 && errno == EINTR);
        if (result != 0) {
            close(fd);
            throw std::runtime_error("cannot lock player");
        }
    }
    ~TrackLock() { close(fd); }
    TrackLock(const TrackLock&) = delete;
    TrackLock& operator=(const TrackLock&) = delete;

    void claim() const
    {
        const auto owner = getpid();
        if (pwrite(fd, &owner, sizeof owner, 0) != sizeof owner)
            throw std::runtime_error("cannot record player stream owner");
    }

    bool isOwner() const
    {
        pid_t owner{};
        const auto bytes = pread(fd, &owner, sizeof owner, 0);
        if (bytes < 0)
            throw std::runtime_error("cannot read player stream owner");
        return bytes == sizeof owner && owner == getpid();
    }

private:
    int fd;
};

class Settings {
public:
    void reload(const std::string& path)
    {
        std::ifstream file(path);
        std::map<std::string, double> replacement;
        std::string name;
        double value;
        while (file >> name >> value)
            replacement[name] = value;
        values.swap(replacement);
    }

    double value(const char* key, double fallback, double low, double high) const
    {
        const auto found = values.find(key);
        if (found == values.end() || !std::isfinite(found->second))
            return fallback;
        return std::clamp(found->second, low, high);
    }

private:
    std::map<std::string, double> values;
};

class History {
public:
    explicit History(int rate) : buffer(rate + blockSize) {}

    void add(Stereo frame)
    {
        buffer[count % buffer.size()] = frame;
        ++count;
    }

    std::vector<Stereo> restore(const std::string& path, int rate, double epoch) const
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return {};
        Header header{};
        file.read(reinterpret_cast<char*>(&header), sizeof header);
        if (!file || header.rate != rate || header.epoch != epoch
            || header.frames > buffer.size())
            return {};
        std::vector<Stereo> prefix(header.frames);
        file.read(reinterpret_cast<char*>(prefix.data()), prefix.size() * sizeof(Stereo));
        if (!file)
            return {};
        return prefix;
    }

    void save(const std::string& path, int rate, double epoch) const
    {
        // Start the prefix at an internal frame boundary so the next track
        // continues with the same covariance block phase.
        const auto oldest = count - std::min<std::uint64_t>(count, buffer.size());
        const auto start = ((oldest + blockSize - 1) / blockSize) * blockSize;
        const Header header{rate, static_cast<std::uint32_t>(count - start), epoch};
        std::ofstream file(path + ".new", std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(&header), sizeof header);
        for (auto i = start; i < count; ++i)
            file.write(reinterpret_cast<const char*>(&buffer[i % buffer.size()]), sizeof(Stereo));
        file.close();
        if (file)
            std::rename((path + ".new").c_str(), path.c_str());
    }

private:
    struct Header {
        int rate;
        std::uint32_t frames;
        double epoch;
    };

    std::vector<Stereo> buffer;
    std::uint64_t count = 0;
};

class Renderer {
public:
    Renderer(int sampleRate, const std::string& base, const std::string& stream)
        : rate(sampleRate), state(base), processor(rate, blockSize), history(rate)
    {
        config.nae_enabled = config.xtc_enabled = true;
        config.xtc.filter_len = NatAmbioConstants::recommendedXtcFilterLen(rate);
        settings.reload(state + ".settings");
        config.nae.steps_length = blockSize * static_cast<int>(settings.value("window", 5, 1, 20));
        updateSettings(true);
        if (!stream.empty()) {
            const auto streamPath = state + "." + stream;
            timelinePath = streamPath + ".timeline";
            // Lyrion uses this marker for live DSP changes, independently of metering.
            std::ofstream rendering(streamPath + ".rendering");
        }
        processor.setPcaReporting(false);
        latency = processor.getLatencySamples(config);
        outputRevision = pendingRevision;
        outputGeometry = config.xtc;
        dry.assign(latency, {});
        drop = latency;
        epoch = settings.value("epoch", 0, 0, 1e15);
        peakRelease = std::pow(10.0, -24.0 / (20.0 * rate));
        rmsAttack = 1 - std::exp(-1.0 / (0.010 * rate));
        rmsRelease = 1 - std::exp(-1.0 / (0.090 * rate));
    }

    void run(const Au& au, bool discontinuity)
    {
        const auto historyPath = state + ".history";
        std::vector<Stereo> prefix;
        {
            TrackLock lock(state + ".lock");
            if (lock.isOwner()) {
                if (!discontinuity && !settings.value("reset", 0, 0, 1))
                    prefix = history.restore(historyPath, rate, epoch);
                std::remove(historyPath.c_str());
            }
        }

        // Warm up without holding the lock or waiting for the prior stream.
        for (const auto& frame : prefix) {
            history.add(frame);
            enqueue(frame, false);
        }
        render(false);

        writeHeader(au);
        for (std::uint64_t i = 0; au.indefinite || i < au.frames; ++i) {
            if (std::cin.peek() == EOF) {
                if (!au.indefinite)
                    throw std::runtime_error("truncated AU track");
                break;
            }
            const float left = readSample(std::cin, au.bytes);
            const float right = au.channels == 2 ? readSample(std::cin, au.bytes) : left;
            const Stereo frame{left, right};
            history.add(frame);
            enqueue(frame, true);
        }

        for (int i = 0; i < latency; ++i)
            enqueue({}, true);
        render(true);
        std::cout.flush();
        if (!std::cout)
            throw std::runtime_error("audio output closed");
        updateViewer(true);
        if (metering && framesRendered > firstMeterFrame)
            report();
        {
            TrackLock lock(state + ".lock");
            settings.reload(state + ".settings");
            if (lock.isOwner() && epoch == settings.value("epoch", 0, 0, 1e15))
                history.save(historyPath, rate, epoch);
        }
    }

private:
    void updateViewer(bool force = false)
    {
        const auto now = Clock::now();
        if (!force && now < nextViewerCheck)
            return;
        nextViewerCheck = now + viewerCheckInterval;

        std::uint64_t session = 0;
        if (timelinePath.empty()) {
            // Keep standalone helper telemetry available to existing callers.
            session = 1;
        } else {
            std::ifstream lease(state + ".viewing");
            double expires = 0;
            const auto wallTime = std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (!(lease >> session >> expires) || expires <= wallTime)
                session = 0;
        }
        if (session == viewerSession)
            return;

        viewerSession = session;
        metering = session != 0;
        processor.setPcaReporting(metering);
        if (metering) {
            cpu = peak = 0;
            energy = {};
            peakHold = 0;
            clipped = false;
            overBudget = 0;
            firstMeterFrame = framesRendered;
            nextReport = framesRendered;
            previousSlot = std::numeric_limits<std::uint64_t>::max();
        }
    }

    void updateSettings(bool initial)
    {
        const double preamp = settings.value("preamp", 0, -18, 0);
        config.nae.gain_main = preamp + settings.value("front", 0, -6, 6);
        config.nae.gain_amb = preamp + settings.value("ambience", 0, -6, 6);
        config.nae.gain_surr = preamp;
        config.nae_bypassed = settings.value("nae_bypass", 0, 0, 1);
        config.xtc_bypassed = settings.value("xtc_bypass", 0, 0, 1);
        const auto azimuth = static_cast<int>(settings.value("azimuth", 30, 10, 30));
        const auto itd = static_cast<int>(settings.value("itd", 260, 80, 300));
        const double ild = settings.value("ild", 14, 3, 20);
        const double alpha = settings.value("alpha", 1.8, 1, 2.5);
        const double slope = settings.value("slope", 200, 60, 500);
        const auto algorithm = settings.value("algorithm", 1, 0, 1) == 1
            ? XtcAlgorithm::Corrected : XtcAlgorithm::Legacy;
        if (initial || azimuth != config.xtc.azimuth_deg || itd != config.xtc.itd_us
            || ild != config.xtc.ild_db || alpha != config.xtc.ild_alpha
            || slope != config.xtc.ild_slope_start_hz || algorithm != config.xtc.algorithm) {
            config.xtc.azimuth_deg = azimuth;
            config.xtc.itd_us = itd;
            config.xtc.ild_db = ild;
            config.xtc.ild_alpha = alpha;
            config.xtc.ild_slope_start_hz = slope;
            config.xtc.algorithm = algorithm;
            if (initial) {
                processor.configureXtc(config.xtc);
            } else {
                auto replacement = std::make_unique<XtcProcessor>(rate, blockSize);
                replacement->configure(config.xtc);
                processor.setXtcProcessor(std::move(replacement));
            }
        }
        trimTarget = std::pow(10.0, settings.value("trim", -3, -12, 6) / 20.0);
        bypassTarget = settings.value("bypass", 0, 0, 1);
        const int revision = static_cast<int>(settings.value("revision", 0, 0, 2147483647));
        if (revision != pendingRevision) {
            pendingRevision = revision;
            revisionDelay = latency + blockSize;
        }
        if (initial) {
            trim = trimTarget;
            bypass = bypassTarget;
        }
    }

    void enqueue(Stereo frame, bool output)
    {
        left[queued] = frame[0];
        right[queued] = frame[1];
        if (++queued == blockSize)
            render(output);
    }

    void render(bool output)
    {
        if (queued == 0)
            return;
        if (output)
            updateViewer();
        const auto started = metering ? Clock::now() : Clock::time_point{};
        if (output && framesRendered >= nextSettingsRead) {
            settings.reload(state + ".settings");
            updateSettings(false);
            nextSettingsRead = framesRendered + rate / 15;
        }
        const double dspStarted = metering ? threadCpuSeconds() : 0;
        processor.process(left.data(), right.data(), wetLeft.data(), wetRight.data(), queued, config);
        if (metering) {
            const double blockSeconds = static_cast<double>(queued) / rate;
            const double load = (threadCpuSeconds() - dspStarted) / blockSeconds;
            cpu += (load - cpu) * -std::expm1(-blockSeconds);
        }
        const double trimCoeff = 1 - std::exp(-1.0 / (0.015 * rate));
        const double bypassStep = 1.0 / (0.015 * rate);
        for (int i = 0; i < queued; ++i) {
            const Stereo dryFrame = dry[dryIndex];
            dry[dryIndex] = {left[i], right[i]};
            dryIndex = (dryIndex + 1) % dry.size();
            if (!output)
                continue;
            if (drop > 0) {
                --drop;
                continue;
            }
            trim += (trimTarget - trim) * trimCoeff;
            bypass += std::clamp(bypassTarget - bypass, -bypassStep, bypassStep);
            emit(0, wetLeft[i], dryFrame[0]);
            emit(1, wetRight[i], dryFrame[1]);
            if (outputRevision != pendingRevision && revisionDelay > 0 && --revisionDelay == 0) {
                outputRevision = pendingRevision;
                appliedFrame = framesRendered;
                outputGeometry = config.xtc;
            }
            if (metering) {
                if (peakHold > 0)
                    --peakHold;
                else
                    peak *= peakRelease;
            }
            ++framesRendered;
        }
        if (metering) {
            const double load = std::chrono::duration<double>(Clock::now() - started).count()
                * rate / queued;
            if (load > 1)
                ++overBudget;
            updateViewer();
            if (metering && framesRendered > firstMeterFrame && framesRendered >= nextReport) {
                report();
                nextReport = (framesRendered / reportInterval() + 1) * reportInterval();
            }
        }
        queued = 0;
    }

    void emit(int channel, float wet, float drySample)
    {
        const double processed = std::isfinite(wet) ? wet * trim : 0;
        const double out = processed * (1 - bypass) + drySample * bypass;
        if (metering) {
            const double magnitude = std::abs(processed);
            if (magnitude >= peak) {
                peak = magnitude;
                peakHold = rate / 12;
            }
            clipped = clipped || magnitude >= 1.0;
            const double squared = out * out;
            energy[channel] += (squared - energy[channel])
                * (squared > energy[channel] ? rmsAttack : rmsRelease);
        }
        const auto quantized = static_cast<std::int32_t>(
            std::llround(std::clamp(out, -1.0, 8388607.0 / 8388608) * 8388608));
        writeWord(std::cout, static_cast<std::uint32_t>(quantized), 3);
    }

    int reportInterval() const { return (rate + 29) / 30; }

    void report()
    {
        const auto pca = processor.getNaePcaAnalysis();
        std::ostringstream json;
        const auto slot = framesRendered / reportInterval();
        json << "{\"rate\":" << rate << ",\"frames\":" << framesRendered
             << ",\"slot\":" << slot << ",\"revision\":" << outputRevision
             << ",\"viewerSession\":" << viewerSession
             << ",\"appliedFrame\":" << appliedFrame
             << ",\"azimuth\":" << outputGeometry.azimuth_deg
             << ",\"itd\":" << outputGeometry.itd_us
             << ",\"ild\":" << outputGeometry.ild_db
             << ",\"alpha\":" << outputGeometry.ild_alpha
             << ",\"block\":" << blockSize << ",\"cpu\":" << cpu
             << ",\"xrun\":" << overBudget << ",\"peak\":" << peak
             << ",\"clipped\":" << (clipped ? "true" : "false")
             << ",\"left\":" << std::sqrt(energy[0]) << ",\"right\":" << std::sqrt(energy[1])
             << ",\"valid\":" << (pca.valid ? "true" : "false")
             << ",\"mid\":" << pca.principalMid << ",\"side\":" << pca.principalSide
             << ",\"major\":" << pca.eigenvaluePrimary << ",\"minor\":" << pca.eigenvalueSecondary
             << ",\"window\":" << pca.windowSamples << "}";
        const auto data = json.str();
        if (!timelinePath.empty() && !timeline.is_open())
            timeline.open(timelinePath, std::ios::binary | std::ios::trunc);
        if (timeline.is_open() && timeline && data.size() < timelineRecordBytes) {
            auto record = data;
            record.resize(timelineRecordBytes - 1, ' ');
            record.push_back('\n');
            if (slot != previousSlot) {
                timeline.seekp((1 + slot % timelineSlots) * timelineRecordBytes);
                timeline.write(record.data(), record.size());
                previousSlot = slot;
            }
            timeline.seekp(0);
            timeline.write(record.data(), record.size());
            timeline.flush();
            return;
        }
        if (!timelinePath.empty())
            return;
        TrackLock lock(state + ".lock");
        if (!lock.isOwner())
            return;
        std::ofstream file(state + ".telemetry.new", std::ios::trunc);
        file << data << '\n';
        file.close();
        if (file)
            std::rename((state + ".telemetry.new").c_str(), (state + ".telemetry").c_str());
    }

    int rate;
    std::string state;
    std::string timelinePath;
    std::ofstream timeline;
    NatAmbioCoreProcessor processor;
    History history;
    Settings settings;
    NatAmbioStereoConfig config;
    XtcStereoConfig outputGeometry;
    double epoch = 0;
    int latency = 0;
    int drop = 0;
    int queued = 0;
    std::vector<Stereo> dry;
    std::size_t dryIndex = 0;
    std::array<float, blockSize> left{}, right{}, wetLeft{}, wetRight{};
    std::array<double, 2> energy{};
    double trim = 1, trimTarget = 1, bypass = 0, bypassTarget = 0;
    double cpu = 0, peak = 0;
    double peakRelease = 0, rmsAttack = 0, rmsRelease = 0;
    int peakHold = 0;
    bool clipped = false;
    bool metering = false;
    Clock::time_point nextViewerCheck{};
    std::uint64_t viewerSession = 0, firstMeterFrame = 0;
    std::uint64_t appliedFrame = 0;
    std::uint64_t previousSlot = std::numeric_limits<std::uint64_t>::max();
    int outputRevision = 0, pendingRevision = 0, revisionDelay = 0;
    std::uint64_t framesRendered = 0, nextReport = 0, nextSettingsRead = 0, overBudget = 0;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::cout << "NatAmbio Alpha 0.1.14\n";
        return 0;
    }
    if (argc < 2) {
        std::cerr << "Usage: natambio-alpha PLAYER_STATE_PATH [seek]\n";
        return 2;
    }

    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    try {
        std::string stream;
        bool discontinuity = false;
        for (int i = 2; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument.compare(0, 9, "--stream=") == 0) {
                stream = argument.substr(9);
                if (stream.empty() || stream.size() > 18
                    || stream.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("invalid stream identifier");
            } else {
                discontinuity = true;
            }
        }
        const std::string state = argv[1];
        {
            // Only the latest helper can update shared history and fallback telemetry.
            TrackLock lock(state + ".lock");
            lock.claim();
        }
        const Au au = readHeader(std::cin);
        Renderer renderer(au.rate, state, stream);
        renderer.run(au, discontinuity);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NatAmbio Alpha: " << error.what() << '\n';
        return 1;
    }
}
