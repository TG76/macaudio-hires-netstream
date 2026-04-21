// Focal HiRes - AudioServerPlugin for network streaming to Raspberry Pi
// Based on libASPL NetcatDevice example

#include <aspl/Driver.hpp>
#include <aspl/VolumeControl.hpp>

#include <CoreAudio/AudioServerPlugIn.h>

#include <cmath>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// ============================================================================
// Configuration
// ============================================================================

constexpr const char* PiHost = "192.168.1.74";
constexpr UInt16 PiPort = 4953;

constexpr UInt32 SampleRate = 192000;
constexpr UInt32 ChannelCount = 2;
constexpr UInt32 BitsPerChannel = 32;

// Max queued buffers (~500ms at 192kHz with 512-frame callbacks)
constexpr size_t MaxQueuedBuffers = 200;

constexpr char HeaderMagic[4] = {'H', 'R', 'E', 'S'};

// ============================================================================
// Float32 -> S32LE conversion (uses double to avoid overflow, handles NaN)
// ============================================================================

inline void ConvertFloat32ToS32LE(const float* input, int32_t* output, size_t sampleCount)
{
    for (size_t i = 0; i < sampleCount; i++) {
        float f = input[i];
        // NaN check: NaN != NaN
        if (f != f) {
            output[i] = 0;
            continue;
        }
        double sample = static_cast<double>(f);
        if (sample > 1.0)
            sample = 1.0;
        if (sample < -1.0)
            sample = -1.0;
        output[i] = static_cast<int32_t>(sample * 2147483647.0);
    }
}

// ============================================================================
// Protocol header (16 bytes)
// ============================================================================

#pragma pack(push, 1)
struct ProtocolHeader
{
    char magic[4];
    uint32_t rate;
    uint32_t bits;
    uint32_t channels;
};
#pragma pack(pop)

// ============================================================================
// Thread-safe audio buffer queue
// ============================================================================

class AudioQueue
{
public:
    void Push(const void* data, size_t bytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= MaxQueuedBuffers) {
            queue_.pop_front(); // drop oldest to prevent unbounded growth
        }
        queue_.emplace_back(
            reinterpret_cast<const uint8_t*>(data),
            reinterpret_cast<const uint8_t*>(data) + bytes);
        cv_.notify_one();
    }

    // Returns empty vector if stopped
    std::vector<uint8_t> Pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(10),
            [this] { return !queue_.empty() || stopped_; });
        if (queue_.empty())
            return {};
        auto buf = std::move(queue_.front());
        queue_.pop_front();
        return buf;
    }

    void Reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        stopped_ = false;
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> queue_;
    bool stopped_ = false;
};

// ============================================================================
// IO Handler with TCP streaming
// Applies dB-taper volume manually (libASPL's default is linear scalar).
// ============================================================================

class FocalHiResHandler : public aspl::ControlRequestHandler, public aspl::IORequestHandler
{
public:
    void SetVolumeControl(std::shared_ptr<aspl::VolumeControl> vc)
    {
        volumeControl_ = std::move(vc);
    }

    OSStatus OnStartIO() override
    {
        queue_.Reset();
        running_.store(true, std::memory_order_release);
        senderThread_ = std::thread(&FocalHiResHandler::SenderLoop, this);
        return kAudioHardwareNoError;
    }

    void OnStopIO() override
    {
        running_.store(false, std::memory_order_release);
        queue_.Stop();
        if (senderThread_.joinable()) {
            senderThread_.join();
        }
    }

    void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>& stream,
        Float64 zeroTimestamp,
        Float64 timestamp,
        const void* buff,
        UInt32 buffBytesSize) override
    {
        const size_t sampleCount = buffBytesSize / sizeof(float);
        std::vector<uint8_t> scaled(buffBytesSize);
        const float* src = reinterpret_cast<const float*>(buff);
        float* dst = reinterpret_cast<float*>(scaled.data());

        float gain = 1.0f;
        if (volumeControl_) {
            const float dB = volumeControl_->GetDecibelValue();
            gain = (dB <= -60.0f) ? 0.0f : std::pow(10.0f, dB / 20.0f);
        }

        for (size_t i = 0; i < sampleCount; i++) {
            float s = src[i] * gain;
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;
            dst[i] = s;
        }

        queue_.Push(scaled.data(), scaled.size());
    }

private:
    void SenderLoop()
    {
        int sock = -1;
        bool headerSent = false;
        std::vector<int32_t> s32Buf;

        while (running_.load(std::memory_order_acquire)) {
            if (sock == -1) {
                sock = TryConnect();
                headerSent = false;
                if (sock == -1) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
            }

            if (!headerSent) {
                ProtocolHeader hdr;
                std::memcpy(hdr.magic, HeaderMagic, 4);
                hdr.rate = SampleRate;
                hdr.bits = BitsPerChannel;
                hdr.channels = ChannelCount;

                if (SendAll(sock, &hdr, sizeof(hdr)) < 0) {
                    close(sock);
                    sock = -1;
                    continue;
                }
                headerSent = true;
            }

            auto floatData = queue_.Pop();
            if (floatData.empty())
                continue;

            size_t sampleCount = floatData.size() / sizeof(float);
            s32Buf.resize(sampleCount);

            ConvertFloat32ToS32LE(
                reinterpret_cast<const float*>(floatData.data()),
                s32Buf.data(),
                sampleCount);

            if (SendAll(sock, s32Buf.data(), sampleCount * sizeof(int32_t)) < 0) {
                close(sock);
                sock = -1;
                continue;
            }
        }

        if (sock != -1) {
            close(sock);
        }
    }

    static int TryConnect()
    {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == -1)
            return -1;

        int flag = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        int bufSize = 256 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(bufSize));

        sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PiPort);
        inet_pton(AF_INET, PiHost, &addr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
            close(sock);
            return -1;
        }

        return sock;
    }

    static ssize_t SendAll(int sock, const void* data, size_t len)
    {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data);
        size_t remaining = len;
        while (remaining > 0) {
            ssize_t sent = send(sock, ptr, remaining, 0);
            if (sent <= 0)
                return -1;
            ptr += sent;
            remaining -= sent;
        }
        return static_cast<ssize_t>(len);
    }

    AudioQueue queue_;
    std::atomic<bool> running_{false};
    std::thread senderThread_;
    std::shared_ptr<aspl::VolumeControl> volumeControl_;
};

// ============================================================================
// Driver creation
// ============================================================================

std::shared_ptr<aspl::Driver> CreateDriver()
{
    auto context = std::make_shared<aspl::Context>();

    aspl::DeviceParameters deviceParams;
    deviceParams.Name = "Focal HiRes";
    deviceParams.SampleRate = SampleRate;
    deviceParams.ChannelCount = ChannelCount;
    deviceParams.EnableMixing = true;

    auto device = std::make_shared<aspl::Device>(context, deviceParams);

    aspl::StreamParameters streamParams;
    streamParams.Format = {
        .mSampleRate = static_cast<Float64>(SampleRate),
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian |
                        kAudioFormatFlagIsPacked,
        .mBytesPerPacket = ChannelCount * sizeof(float),
        .mFramesPerPacket = 1,
        .mBytesPerFrame = ChannelCount * sizeof(float),
        .mChannelsPerFrame = ChannelCount,
        .mBitsPerChannel = 32,
    };

    device->AddStreamAsync(streamParams);

    aspl::VolumeControlParameters volParams;
    volParams.Scope = kAudioObjectPropertyScopeOutput;
    volParams.MinRawVolume = 0;
    volParams.MaxRawVolume = 96;
    volParams.MinDecibelVolume = -96.0f;
    volParams.MaxDecibelVolume = 0.0f;

    auto volumeControl = device->AddVolumeControlAsync(volParams);

    std::vector<AudioValueRange> rates = {
        {44100.0, 44100.0},
        {48000.0, 48000.0},
        {96000.0, 96000.0},
        {192000.0, 192000.0},
    };
    device->SetAvailableSampleRatesAsync(rates);

    auto handler = std::make_shared<FocalHiResHandler>();
    handler->SetVolumeControl(volumeControl);
    device->SetControlHandler(handler);
    device->SetIOHandler(handler);

    auto plugin = std::make_shared<aspl::Plugin>(context);
    plugin->AddDevice(device);

    return std::make_shared<aspl::Driver>(context, plugin);
}

} // namespace

extern "C" void* FocalHiResEntryPoint(CFAllocatorRef allocator, CFUUIDRef typeUUID)
{
    if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
        return nullptr;
    }

    static std::shared_ptr<aspl::Driver> driver = CreateDriver();
    return driver->GetReference();
}
