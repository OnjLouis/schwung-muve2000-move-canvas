#include "plugin_api_v1.h"
#include "PlayerWorkerPolicy.h"
#include "smf.h"
#include "ui/xg_ui.h"
#include "vst3/engine.h"
#include "xg/fx_types.h"
#include "xg/voices.h"
#include "../common/ProgramList.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <unordered_set>

namespace {
std::mutex voiceMutex;
std::shared_ptr<const std::vector<u8>> voiceData;

auto voiceRom() {
    const std::lock_guard lock(voiceMutex);
    return xg::voice_rom(voiceData);
}
}

void ui::xgui::set_voice_rom(std::shared_ptr<const std::vector<u8>> rom) {
    static std::once_flag types;
    if (rom) std::call_once(types, [&] { xg::set_fx_type_rom(*rom); });
    const std::lock_guard lock(voiceMutex);
    if (rom) voiceData = std::move(rom);
}

namespace {
using smu2000::vst3::status;
constexpr auto sampleRate = MOVE_SAMPLE_RATE;
constexpr int maximumMidiBytes = 16 * 1024 * 1024;
constexpr int midiParts = 16;
constexpr int midiPorts = 4;
constexpr int octaveLimit = 4, semitonesPerOctave = 12, midiNotes = 128;
#ifndef MU2000_RENDER_AHEAD
#define MU2000_RENDER_AHEAD 6
#endif
constexpr unsigned renderAhead = MU2000_RENDER_AHEAD;
static_assert(renderAhead >= 2 && renderAhead <= 32);
// The file worker can run ahead of a dense passage. Idle keyboard input and
// the separate instrument retain their short interactive queues.
#if MU2000_PLAYER
constexpr unsigned liveFileBlocks = 6 * sampleRate / MOVE_FRAMES_PER_BLOCK;
constexpr unsigned initialFileBlocks = sampleRate / MOVE_FRAMES_PER_BLOCK;
constexpr unsigned queueCapacity = std::max(renderAhead, liveFileBlocks);
#else
constexpr unsigned queueCapacity = renderAhead;
constexpr unsigned fastPadBlocks = std::min(3u, renderAhead);
#endif
constexpr int edgeFrames = 32;
constexpr auto knobs = std::to_array<std::pair<std::string_view, uint8_t>>({
    {"volume", 7}, {"pan", 10}, {"cutoff", 74}, {"resonance", 71},
    {"attack", 73}, {"release", 72}, {"reverb", 91}, {"chorus", 93}
});
constexpr std::array<int, knobs.size()> defaultKnobs{100, 64, 64, 64, 64, 64, 40, 0};
constexpr unsigned kitAddress(int bank) { return unsigned(bank) << 14; }
constexpr std::array<unsigned, 3> defaultGroups{0, kitAddress(mu_pilot::drumKitBank), kitAddress(mu_pilot::sfxKitBank)};
constexpr size_t groupIndex(unsigned address) {
    const auto bank = address >> 14;
    return bank == mu_pilot::drumKitBank ? 1 : bank == mu_pilot::sfxKitBank ? 2 : 0;
}
constexpr unsigned queuedMidi = 256;
constexpr std::string_view library = "/data/UserData/UserLibrary/";

struct Song {
    std::vector<smf::event> events;
    std::string name;
};
constexpr unsigned seekBatch = 24;
constexpr int unityTrim = 100, maxTrim = 100;
constexpr unsigned overloadBlocksPerSecond = 64;
constexpr unsigned overloadWindowsBeforeStop = 3;
#if MU2000_PLAYER
constexpr unsigned renderedLimitSeconds = 600, renderedTailSeconds = 2;
constexpr char playbackModeFile[] = "mu2000-midi-player-mode";
enum class PlaybackMode { render, live };
std::filesystem::path playerConfig() {
    if (const auto* root = std::getenv("XDG_CONFIG_HOME"); root && *root)
        return std::filesystem::path(root) / "schwung";
    return "/data/UserData/schwung/config";
}
PlaybackMode readPlaybackMode() {
    std::ifstream input(playerConfig() / playbackModeFile);
    std::string line;
    return std::getline(input, line) && line == "live" ? PlaybackMode::live : PlaybackMode::render;
}
bool savePlaybackMode(PlaybackMode mode) {
    static std::mutex preferencesMutex;
    const std::lock_guard lock(preferencesMutex);
    const auto config = playerConfig();
    std::error_code error;
    std::filesystem::create_directories(config, error);
    if (error) return false;
    const auto target = config / playbackModeFile;
    auto temporary = target;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << (mode == PlaybackMode::live ? "live\n" : "render\n");
    output.close();
    if (output) {
        std::filesystem::rename(temporary, target, error);
        if (!error) return true;
    }
    std::filesystem::remove(temporary, error);
    return false;
}
constexpr unsigned framesPerBlock = MOVE_FRAMES_PER_BLOCK;
constexpr unsigned renderBlockCapacity(unsigned durationMs) {
    const auto frames = (uint64_t(sampleRate) * durationMs + 999) / 1000 + sampleRate * renderedTailSeconds;
    // The public duration is rounded down; allow for that and the final block.
    return unsigned((frames + framesPerBlock - 1) / framesPerBlock) + 2;
}
enum class RenderedMode { off, requested, preparing, playing, paused, finished };
enum class RenderFailure { none, memory, capacity };
#endif

struct Instance {
    smu2000::vst3::engine engine;
    std::array<float, MOVE_FRAMES_PER_BLOCK> left{}, right{};
    std::mutex songMutex;
    Song song;
    std::atomic<bool> playing{false}, rewind{false}, stop{false};
    std::atomic<bool> loop{false}, usb{true}, ended{false}, seeking{false};
    std::atomic<bool> overloaded{false};
    std::atomic<int> seekMillis{-1}, master{unityTrim};
    std::atomic<unsigned> positionMs{0}, durationMs{0}, cpuAvg{0}, cpuPeak{0}, voices{0};
    struct MixPart {
        std::atomic<int> trim{unityTrim};
        std::atomic<bool> mute{false}, solo{false}, changed{false}, used{false};
        int midiVolume = 100;
        bool silent = false;
    };
    std::array<MixPart, midiPorts * midiParts> mix{};
    uint64_t seekTarget = 0;
    unsigned peakHold = 0, budgetBlocks = 0, budgetGaps = 0, overloadWindows = 0;
    inline static std::atomic<unsigned> nextEditorId{1};
    const unsigned editorId = nextEditorId.fetch_add(1);
    std::atomic<int> part{0}, octave{0}, trackChannel{-1};
    std::atomic<bool> followTrack{true};
#if !MU2000_PLAYER
    std::atomic<bool> fastPads{true};
#endif
    struct Part {
        std::atomic<unsigned> address{0};
        std::array<std::atomic<unsigned>, 3> lastGroup{defaultGroups[0], defaultGroups[1], defaultGroups[2]};
        std::array<std::atomic<int>, knobs.size()> values{100, 64, 64, 64, 64, 64, 40, 0};
        std::atomic<uint32_t> dirty{0};
    };
    std::array<Part, midiPorts * midiParts> parts{};
    std::atomic<unsigned> voiceMode{0x101};
    unsigned snapshotDivider = 0;
    std::array<uint16_t, midiPorts> used{};
    std::string fileError;
    uint64_t position = 0;
    size_t event = 0;
    struct Message { int size; int source; std::array<uint8_t, 256> data; };
    using HeldNotes = std::array<std::array<int16_t, midiNotes>, midiParts>;
    std::array<HeldNotes, 2> liveNotes{};
    std::array<Message, queuedMidi> queue{};
    std::atomic<unsigned> midiRead{0}, midiWrite{0};
    using Block = std::array<int16_t, MOVE_FRAMES_PER_BLOCK * 2>;
    std::array<Block, queueCapacity> audio{};
#if MU2000_PLAYER
    std::atomic<PlaybackMode> playbackMode{readPlaybackMode()};
    std::atomic<unsigned> audioGeneration{0};
    std::atomic<bool> primingFile{false}, fileAudible{false};
    std::atomic<unsigned> heardMs{0};
    std::array<unsigned, queueCapacity> blockPosition{};
    std::array<bool, queueCapacity> blockEnded{};
    std::array<unsigned, queueCapacity> blockGeneration{};
    std::unique_ptr<Block[]> cachedAudio;
    std::atomic<RenderedMode> rendered{RenderedMode::off};
    std::atomic<RenderFailure> renderFailure{RenderFailure::none};
    std::atomic<bool> cacheRead{false};
    std::atomic<unsigned> cachedCount{0}, cachedRead{0};
    unsigned cachedTail = 0, cachedCapacity = 0;
#endif
    std::atomic<unsigned> audioRead{0}, audioWrite{0}, underruns{0};
    std::atomic<bool> activated{false};
    std::atomic<unsigned> clipped{0}, midiDrops{0};
    int16_t lastLeft = 0, lastRight = 0;
    bool recovering = false;
#if MU2000_ABI_PROBE
    const bool synchronousProbe = std::getenv("MU2000_SYNCHRONOUS_PROBE") != nullptr;
#endif
    std::jthread worker;
};

void updateMix(Instance& instance, size_t i) {
    const auto& part = instance.mix[i];
    const auto channel = uint8_t(0xb0 | int(i % midiParts));
    const uint8_t volume[]{channel, 7, uint8_t(part.silent ? 0 : part.midiVolume * part.trim.load() / unityTrim)};
    instance.engine.midi(volume, sizeof volume, int(i / midiParts));
}

void send(Instance& instance, const uint8_t* bytes, size_t length, int port) {
    if (length == 0 || port < 0 || port >= midiPorts) return;
#if MU2000_PLAYER
    if (length == 3 && (bytes[0] & 0xf0) == 0xb0 && bytes[1] == 7) {
        const auto i = size_t(port * midiParts + (bytes[0] & 15));
        instance.mix[i].midiVolume = bytes[2] & 127;
        updateMix(instance, i);
        return;
    }
#endif
    if (length >= 3 && (bytes[0] & 0xf0) == 0x90 && bytes[2] != 0)
        instance.used[size_t(port)] |= uint16_t(1u << (bytes[0] & 15));
    instance.engine.midi(bytes, length, port);
}

void silence(Instance& instance) {
    instance.engine.all_notes_off(instance.used.data(), midiPorts);
    instance.used.fill(0);
    for (auto& input : instance.liveNotes)
        for (auto& channel : input) channel.fill(0);
}

#if MU2000_PLAYER
void requestTransportStop(Instance& instance) {
    instance.fileAudible = false;
    instance.primingFile = false;
    instance.playing = false;
    instance.stop.store(true, std::memory_order_release);
    instance.audioGeneration.fetch_add(1, std::memory_order_release);
}

void silenceTransport(Instance& instance) {
    // Release pedal and sostenuto state as well as notes on sounded parts.
    for (int port = 0; port < midiPorts; ++port)
        for (int ch = 0; ch < midiParts; ++ch) {
            if (!(instance.used[size_t(port)] & (1u << ch))) continue;
            const auto status = uint8_t(0xb0 | ch);
            const uint8_t pedals[]{status, 64, 0, status, 66, 0};
            instance.engine.midi(pedals, sizeof pedals, port);
        }
    silence(instance);
}
#endif

void sendLive(Instance& instance, const Instance::Message& message) {
    const auto* bytes = message.data.data();
    const int type = bytes[0] & 0xf0;
    const bool external = message.source == MOVE_MIDI_SOURCE_EXTERNAL;
    if ((!external && message.source != MOVE_MIDI_SOURCE_INTERNAL) || message.size != 3 ||
        (type != 0x80 && type != 0x90 && type != 0xa0) || bytes[1] >= midiNotes) {
        send(instance, bytes, size_t(message.size), 0);
        return;
    }
    // A release follows the note-on pitch even if Octave changed meanwhile.
    auto& held = instance.liveNotes[external ? 1 : 0][bytes[0] & 15][bytes[1]];
    const bool noteOn = type == 0x90 && bytes[2];
    const int target = int(bytes[1]) + instance.octave.load(std::memory_order_relaxed) * semitonesPerOctave;
    if (noteOn && held > 0) {
        const uint8_t off[]{uint8_t(0x80 | (bytes[0] & 15)), uint8_t(held - 1), 0};
        send(instance, off, sizeof off, 0);
    }
    if (noteOn) held = (target >= 0 && target < midiNotes) ? int16_t(target + 1) : int16_t(-1);
    const int note = held > 0 ? held - 1 : held == -1 ? -1 : int(bytes[1]);
    if (type != 0xa0 && !noteOn) held = 0;
    if (note < 0) return;
    const uint8_t mapped[]{bytes[0], uint8_t(note), bytes[2]};
    send(instance, mapped, sizeof mapped, 0);
}

constexpr uint32_t patchBit = 1u << knobs.size();
void edits(Instance& instance) {
    for (size_t i = 0; i < instance.parts.size(); ++i) {
        auto& part = instance.parts[i];
        const auto dirty = part.dirty.exchange(0);
        if (!dirty) continue;
        const int port = int(i) / midiParts;
        const auto channel = uint8_t(i % midiParts);
        if (dirty & patchBit) {
            const auto address = part.address.load();
            const uint8_t bytes[]{uint8_t(0xb0 | channel), 0, uint8_t(address >> 14),
                uint8_t(0xb0 | channel), 32, uint8_t((address >> 7) & 127),
                uint8_t(0xc0 | channel), uint8_t(address & 127)};
            send(instance, bytes, sizeof bytes, port);
        }
        for (size_t k = 0; k < knobs.size(); ++k) {
            if (!(dirty & (1u << k))) continue;
            const uint8_t bytes[]{uint8_t(0xb0 | channel), knobs[k].second, uint8_t(part.values[k].load())};
            send(instance, bytes, sizeof bytes, port);
        }
    }
}

void renderAheadBlock(Instance& instance, int16_t* out, int frames) {
    if (frames <= 0) return;
    std::fill_n(out, 2 * size_t(frames), 0);
    if (instance.engine.state() != status::ready) return;
    auto read = instance.midiRead.load(std::memory_order_relaxed);
    const auto write = instance.midiWrite.load(std::memory_order_acquire);
    while (read != write) {
        const auto& message = instance.queue[read % queuedMidi];
        sendLive(instance, message);
        ++read;
    }
    instance.midiRead.store(read, std::memory_order_release);
    edits(instance);
#if !MU2000_PLAYER
    if (instance.stop.exchange(false)) silence(instance);
#endif
#if MU2000_PLAYER
    const bool anySolo = std::any_of(instance.mix.begin(), instance.mix.end(), [](const auto& part) { return part.solo.load(); });
    for (size_t i = 0; i < instance.mix.size(); ++i) {
        auto& part = instance.mix[i];
        const bool silent = part.mute.load() || (anySolo && !part.solo.load());
        const bool changed = part.changed.exchange(false);
        if (silent != part.silent || changed) {
            part.silent = silent;
            updateMix(instance, i);
        }
    }
#endif
    std::unique_lock lock(instance.songMutex, std::defer_lock);
#if MU2000_PLAYER
    if (instance.playing.load() || instance.seeking.load() || instance.rendered.load() == RenderedMode::preparing) lock.lock();
    else
#endif
        lock.try_lock();
#if MU2000_PLAYER
    if (instance.stop.exchange(false)) silenceTransport(instance);
#endif
    if (lock && instance.rewind.exchange(false)) {
        silence(instance);
        instance.position = 0;
        instance.event = 0;
        instance.positionMs = 0;
        instance.ended = false;
        instance.seeking = false;
    }
#if MU2000_PLAYER
    if (lock) {
        if (const auto target = instance.seekMillis.exchange(-1); target >= 0) {
            silence(instance);
            const uint8_t reset[]{0xf0, 0x43, 0x10, 0x4c, 0, 0, 0x7e, 0, 0xf7};
            instance.engine.midi(reset, sizeof reset, 0);
            for (auto& part : instance.mix) {
                part.midiVolume = 100;
                if (part.silent || part.trim.load() != unityTrim) part.changed = true;
            }
            instance.event = 0;
            instance.seekTarget = std::min(uint64_t(target) * sampleRate / 1000,
                uint64_t(instance.durationMs.load()) * sampleRate / 1000);
            instance.seeking = true;
            instance.ended = false;
        }
        if (instance.seeking) {
            unsigned budget = 0;
            while (instance.event < instance.song.events.size() && budget < seekBatch) {
                const auto& old = instance.song.events[instance.event];
                if (uint64_t(old.time * sampleRate) >= instance.seekTarget) break;
                if (!old.bytes.empty()) {
                    const auto status = old.bytes[0];
                    const auto kind = status & 0xf0;
                    if ((kind >= 0xb0 && kind <= 0xe0) || status == 0xf0)
                        send(instance, old.bytes.data(), old.bytes.size(), old.port);
                }
                ++instance.event;
                ++budget;
            }
            if (budget < seekBatch) {
                instance.position = instance.seekTarget;
                instance.positionMs = unsigned(instance.position * 1000 / sampleRate);
                instance.seeking = false;
            }
        }
    }
#endif
    unsigned clipped = 0;
    bool reachedEnd = false;
    for (int offset = 0; offset < frames;) {
        int count = std::min(frames - offset, int(instance.left.size()));
        if (lock && !reachedEnd && instance.playing.load() && !instance.seeking.load()) {
            while (instance.event < instance.song.events.size()) {
                const auto& next = instance.song.events[instance.event];
                const auto sample = uint64_t(std::max(0.0, next.time) * sampleRate);
                if (sample > instance.position) {
                    count = int(std::min<uint64_t>(uint64_t(count), sample - instance.position));
                    break;
                }
                send(instance, next.bytes.data(), next.bytes.size(), next.port);
                ++instance.event;
            }
            instance.position += uint64_t(count);
            if (instance.event == instance.song.events.size()) {
                reachedEnd = true;
#if MU2000_PLAYER
                if (instance.rendered.load() != RenderedMode::preparing && instance.loop.load() && !instance.song.events.empty()) instance.rewind = true;
                else { instance.playing = false; instance.ended = true; }
#else
                instance.playing = false;
#endif
                // File note-offs and effect tails still have to render.
            }
            instance.positionMs = std::min(instance.durationMs.load(), unsigned(instance.position * 1000 / sampleRate));
        }
        instance.engine.fill(instance.left.data(), instance.right.data(), count);
        // Live master volume is applied at output so it also controls queued audio.
#if MU2000_PLAYER
        const bool liveOutput = instance.rendered.load() == RenderedMode::off;
        const float gain = liveOutput ? 1.f : float(instance.master.load(std::memory_order_relaxed)) / unityTrim;
#else
        const float gain = float(instance.master.load(std::memory_order_relaxed)) / unityTrim;
#endif
        for (int i = 0; i < count; ++i) {
            const auto convert = [&](float sample) -> int16_t {
                if (std::abs(sample) >= 1.0f) ++clipped;
                return std::isfinite(sample) ? int16_t(std::clamp(sample, -1.0f, 1.0f) * 32767.0f) : 0;
            };
            out[2 * (offset + i)] = convert(instance.left[size_t(i)] * gain);
            out[2 * (offset + i) + 1] = convert(instance.right[size_t(i)] * gain);
        }
        offset += count;
    }
    if (clipped) instance.clipped.fetch_add(clipped, std::memory_order_relaxed);
    if ((instance.snapshotDivider++ & 15) == 0) {
        ui::xg_snapshot snapshot;
        instance.engine.panel().read_xg(snapshot);
        if (snapshot.serial) instance.voiceMode = unsigned(snapshot.voice_mode) << 8 | snapshot.voice_set;
#if MU2000_PLAYER
        ui::snapshot panel;
        instance.engine.panel().read(panel);
        instance.voices = unsigned(panel.voices_master) + panel.voices_slave;
#endif
    }
}

void render(void* object, int16_t* out, int frames) {
    auto& instance = *static_cast<Instance*>(object);
    if (frames <= 0) return;
#if MU2000_ABI_PROBE
    if (instance.synchronousProbe) {
        renderAheadBlock(instance, out, frames);
        return;
    }
#endif
#if MU2000_PLAYER
    const auto mode = instance.rendered.load();
    if (mode != RenderedMode::off) {
        std::fill_n(out, 2 * size_t(frames), 0);
        if (mode == RenderedMode::playing && frames == framesPerBlock) {
            instance.cacheRead = true;
                    if (instance.rendered.load() == RenderedMode::playing) {
                auto read = instance.cachedRead.load();
                if (read < instance.cachedCount.load()) {
                    const auto& block = instance.cachedAudio[read];
                    std::copy(block.begin(), block.end(), out);
                    if (instance.cachedRead.compare_exchange_strong(read, read + 1))
                        instance.positionMs = std::min(instance.durationMs.load(), unsigned(uint64_t(read + 1) * framesPerBlock * 1000 / sampleRate));
                } else if (instance.loop.load()) {
                    instance.cachedRead.store(0);
                    if (instance.cachedCount.load()) {
                        const auto& block = instance.cachedAudio[0];
                        std::copy(block.begin(), block.end(), out);
                        instance.cachedRead = 1;
                        instance.positionMs = 0;
                    }
                } else {
                    auto expected = RenderedMode::playing;
                    instance.rendered.compare_exchange_strong(expected, RenderedMode::finished);
                }
            }
            instance.cacheRead = false;
        }
        return;
    }
#endif
    auto read = instance.audioRead.load(std::memory_order_relaxed);
    const auto write = instance.audioWrite.load(std::memory_order_acquire);
#if MU2000_PLAYER
    const auto generation = instance.audioGeneration.load(std::memory_order_acquire);
    const auto previousRead = read;
    while (read != write && instance.blockGeneration[read % queueCapacity] != generation) ++read;
    const bool discarded = previousRead != read;
    if (discarded) {
        instance.audioRead.store(read, std::memory_order_release);
        instance.lastLeft = instance.lastRight = 0;
    }
    if (instance.primingFile.load(std::memory_order_acquire)) {
        if (write - read < initialFileBlocks && !instance.ended.load()) {
            std::fill_n(out, 2 * size_t(frames), 0);
            return;
        }
        instance.primingFile = false;
    }
#endif
    if (frames == MOVE_FRAMES_PER_BLOCK && read != write) {
#if MU2000_PLAYER
        if (instance.fileAudible.load()) {
            instance.heardMs = instance.blockPosition[read % queueCapacity];
            if (instance.blockEnded[read % queueCapacity]) instance.fileAudible = false;
        }
#endif
        const auto& block = instance.audio[read % queueCapacity];
#if MU2000_PLAYER
        const int master = instance.master.load(std::memory_order_relaxed);
        if (master != unityTrim) {
            std::transform(block.begin(), block.end(), out, [master](int16_t sample) {
                return int16_t(int(sample) * master / unityTrim);
            });
        } else
#endif
            std::copy(block.begin(), block.end(), out);
        if (instance.recovering) {
            for (int i = 0; i < edgeFrames; ++i) {
                out[2 * i] = int16_t(int(out[2 * i]) * (i + 1) / edgeFrames);
                out[2 * i + 1] = int16_t(int(out[2 * i + 1]) * (i + 1) / edgeFrames);
            }
            instance.recovering = false;
        }
        instance.lastLeft = out[2 * frames - 2];
        instance.lastRight = out[2 * frames - 1];
        instance.audioRead.store(read + 1, std::memory_order_release);
    } else {
        std::fill_n(out, 2 * size_t(frames), 0);
        if (instance.activated.load(std::memory_order_acquire)
#if MU2000_PLAYER
            && !discarded
#endif
        ) {
            ++instance.underruns;
            const int edge = std::min(frames, edgeFrames);
            for (int i = 0; i < edge; ++i) {
                out[2 * i] = int16_t(int(instance.lastLeft) * (edge - i - 1) / edge);
                out[2 * i + 1] = int16_t(int(instance.lastRight) * (edge - i - 1) / edge);
            }
            instance.lastLeft = instance.lastRight = 0;
            instance.recovering = true;
        }
    }
}

void* create(const char*, const char*) {
    try {
        auto instance = std::make_unique<Instance>();
        instance->engine.set_output_rate(sampleRate);
        instance->engine.start();
#if MU2000_ABI_PROBE
        if (instance->synchronousProbe) return instance.release();
#endif
        instance->worker = std::jthread([state = instance.get()](std::stop_token stop) {
#if defined(__linux__)
            pthread_setname_np(pthread_self(), "mu2000-render");
            mu_player::WorkerPolicy policy;
#endif
            while (!stop.stop_requested()) {
                if (state->engine.state() == status::failed) return;
#if MU2000_PLAYER
                auto mode = state->rendered.load();
                if (mode != RenderedMode::off) {
                    if (state->engine.state() != status::ready) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    if (mode == RenderedMode::requested && !state->cacheRead.load()) {
                        const std::lock_guard lock(state->songMutex);
                        if (state->rendered.load() != RenderedMode::requested) continue;
                        const auto capacity = renderBlockCapacity(state->durationMs.load());
                        if (state->cachedCapacity < capacity) {
                            state->cachedAudio.reset();
                            state->cachedCapacity = 0;
                            state->cachedAudio.reset(new (std::nothrow) Instance::Block[capacity]);
                            if (state->cachedAudio) state->cachedCapacity = capacity;
                        }
                        if (!state->cachedAudio) {
                            state->renderFailure = RenderFailure::memory;
                            state->rendered.compare_exchange_strong(mode, RenderedMode::off);
                            continue;
                        }
                        if (!state->rendered.compare_exchange_strong(mode, RenderedMode::preparing)) continue;
                        state->cachedCount = 0;
                        state->cachedRead = 0;
                        state->cachedTail = 0;
                        state->rewind = true;
                        state->playing = true;
                    }
                    if (state->rendered.load() == RenderedMode::preparing) {
                        const auto count = state->cachedCount.load();
                        if (count >= state->cachedCapacity) {
                            requestTransportStop(*state);
                            state->renderFailure = RenderFailure::capacity;
                            auto expected = RenderedMode::preparing;
                            state->rendered.compare_exchange_strong(expected, RenderedMode::off);
                            continue;
                        }
                        renderAheadBlock(*state, state->cachedAudio[count].data(), framesPerBlock);
                        state->cachedCount = count + 1;
                        if (state->ended.load() && ++state->cachedTail >= renderedTailSeconds * sampleRate / framesPerBlock) {
                            state->cachedRead = 0;
                            state->positionMs = 0;
                            auto expected = RenderedMode::preparing;
                            state->rendered.compare_exchange_strong(expected, RenderedMode::playing);
                        }
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    continue;
                }
#endif
                const auto write = state->audioWrite.load(std::memory_order_relaxed);
                const auto read = state->audioRead.load(std::memory_order_acquire);
                const auto depth =
#if MU2000_PLAYER
                    state->playing.load() ? liveFileBlocks : renderAhead;
#else
                    state->fastPads.load() ? fastPadBlocks : renderAhead;
#endif
                if (state->engine.state() != status::ready || write - read >= depth) {
                    std::this_thread::sleep_for(std::chrono::microseconds(250));
                    continue;
                }
#if MU2000_PLAYER
                const auto gaps = state->underruns.load(std::memory_order_relaxed);
                if (!state->playing.load()) {
                    state->budgetBlocks = 0;
                    state->budgetGaps = gaps;
                    state->overloadWindows = 0;
                } else if (++state->budgetBlocks >= sampleRate / MOVE_FRAMES_PER_BLOCK) {
                    // Allow a short Yamaha setup burst; stop only after repeated starvation.
                    state->overloadWindows = gaps - state->budgetGaps >= overloadBlocksPerSecond ?
                        state->overloadWindows + 1 : 0;
                    if (state->overloadWindows >= overloadWindowsBeforeStop) {
                        state->overloaded = true;
                        requestTransportStop(*state);
                    }
                    state->budgetBlocks = 0;
                    state->budgetGaps = gaps;
                }
                const auto start = std::chrono::steady_clock::now();
                state->blockGeneration[write % queueCapacity] = state->audioGeneration.load(std::memory_order_acquire);
#endif
                renderAheadBlock(*state, state->audio[write % queueCapacity].data(), MOVE_FRAMES_PER_BLOCK);
#if MU2000_PLAYER
                state->blockPosition[write % queueCapacity] = state->positionMs.load();
                state->blockEnded[write % queueCapacity] = state->ended.load() && !state->playing.load();
                const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                const auto load = unsigned(std::min(99900.0, 10000.0 * elapsed * sampleRate / MOVE_FRAMES_PER_BLOCK));
                state->cpuAvg = (state->cpuAvg.load(std::memory_order_relaxed) * 49 + load) / 50;
                if (!state->peakHold || load > state->cpuPeak.load(std::memory_order_relaxed)) {
                    state->cpuPeak = load;
                    state->peakHold = sampleRate / MOVE_FRAMES_PER_BLOCK;
                } else --state->peakHold;
#endif
                state->audioWrite.store(write + 1, std::memory_order_release);
                if (write >= std::min(depth, renderAhead) - 1) state->activated.store(true, std::memory_order_release);
            }
        });
        return instance.release();
    } catch (...) { return nullptr; }
}

void incoming(void* object, const uint8_t* data, int size, int source) {
    if (!data || size <= 0 || source == MOVE_MIDI_SOURCE_FX_BROADCAST) return;
#if MU2000_PLAYER
    auto& player = *static_cast<Instance*>(object);
    if (source != MOVE_MIDI_SOURCE_EXTERNAL || !player.usb.load() || player.rendered.load() != RenderedMode::off) return;
#endif
    auto& instance = *static_cast<Instance*>(object);
    const auto write = instance.midiWrite.load(std::memory_order_relaxed);
#if !MU2000_PLAYER
    if (instance.followTrack && instance.trackChannel < 0 && size == 3 &&
        (data[0] & 0xf0) == 0x90 && data[2])
        instance.part = data[0] & 15;
#endif
    const auto read = instance.midiRead.load(std::memory_order_acquire);
    auto& slot = instance.queue[write % queuedMidi];
    if (write - read >= queuedMidi || size_t(size) > slot.data.size()) {
        ++instance.midiDrops;
        return;
    }
    slot.size = size;
    slot.source = source;
    std::memcpy(slot.data.data(), data, size_t(size));
    instance.midiWrite.store(write + 1, std::memory_order_release);
}

bool loadSong(Instance& instance, const char* value) {
    std::error_code error;
    const auto path = std::filesystem::weakly_canonical(value, error);
    if (error || !path.generic_string().starts_with(library)) return false;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > maximumMidiBytes) return false;
    std::ifstream input(path, std::ios::binary);
    std::vector<u8> bytes(static_cast<size_t>(size), 0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return false;
    Song song;
    std::string problem;
    if (!smf::load_from_memory(bytes.data(), bytes.size(), song.events, problem) || song.events.empty()) return false;
    if (!std::isfinite(song.events.back().time) || song.events.back().time < 0 || song.events.back().time > 24 * 60 * 60)
        return false;
    song.name = path.filename().string();
    const std::lock_guard lock(instance.songMutex);
#if MU2000_PLAYER
    const auto mode = instance.rendered.load();
    if (mode == RenderedMode::preparing || mode == RenderedMode::requested || mode == RenderedMode::playing) {
        instance.fileError = "Stop rendered playback before opening another file.";
        return true;
    }
    instance.heardMs = 0;
    instance.rendered = RenderedMode::off;
    instance.renderFailure = RenderFailure::none;
    requestTransportStop(instance);
#endif
    instance.playing = false;
    instance.rewind = true;
    instance.fileError.clear();
    instance.overloaded = false;
    instance.seekMillis = -1;
    instance.positionMs = 0;
    instance.durationMs = unsigned(song.events.back().time * 1000);
    for (auto& part : instance.mix) part.used = false;
    for (const auto& event : song.events)
        if (event.port < midiPorts && event.bytes.size() == 3 &&
            (event.bytes[0] & 0xf0) == 0x90 && event.bytes[2])
            instance.mix[size_t(event.port * midiParts + (event.bytes[0] & 15))].used = true;
    instance.song = std::move(song);
    return true;
}

std::string patchName(const xg::voice_rom& rom, unsigned mode, int msb, int lsb, int pc) {
    if (!rom.ok()) return {};
    if (mu_pilot::isKitBank(msb)) return rom.kit_name(msb, pc);
    if (!rom.lsb_ok(int(mode >> 8), int(mode & 255), msb, lsb)) return {};
    auto name = rom.record_name(rom.lookup(int(mode >> 8), int(mode & 255), msb, lsb, pc));
    return name == "Silence" ? std::string{} : name;
}

void selectAddress(Instance::Part& part, unsigned address) {
    part.lastGroup[groupIndex(address)].store(address);
    if (part.address.exchange(address) != address) part.dirty.fetch_or(patchBit);
}

// Use the same ROM lookup as Desktop and skip aliases of the same voice.
std::vector<unsigned> variationAddresses(Instance& instance, unsigned address) {
    const auto rom = voiceRom();
    if (!rom.ok()) return {};
    const auto mode = instance.voiceMode.load();
    const int pc = int(address & 127);
    const int voiceMode = int(mode >> 8), voiceSet = int(mode & 255);
    std::unordered_set<u32> seen;
    std::vector<unsigned> choices;
    for (int msb = 0; msb < mu_pilot::sfxKitBank; ++msb) {
        for (int lsb = 0; lsb < 128; ++lsb) {
            if (!rom.lsb_ok(voiceMode, voiceSet, msb, lsb)) continue;
            const auto record = rom.lookup(voiceMode, voiceSet, msb, lsb, pc);
            if (!record || seen.contains(record)) continue;
            const auto name = rom.record_name(record);
            if (name.empty() || name == "Silence") continue;
            seen.insert(record);
            choices.push_back(unsigned(msb << 14 | lsb << 7 | pc));
        }
    }
    return choices;
}

void browseVariations(Instance& instance, Instance::Part& part, int delta) {
    const auto address = part.address.load();
    const auto choices = variationAddresses(instance, address);
    if (choices.empty()) return;
    const auto current = std::find(choices.begin(), choices.end(), address);
    const int index = current == choices.end() ? (delta > 0 ? -1 : int(choices.size())) : int(current - choices.begin());
    selectAddress(part, choices[size_t(std::clamp(index + delta, 0, int(choices.size()) - 1))]);
}

void browse(Instance& instance, Instance::Part& part, int delta) {
    const unsigned address = part.address.load();
    const int msb = int(address >> 14), lsb = int((address >> 7) & 127);
    const auto rom = voiceRom();
    const unsigned mode = instance.voiceMode.load();
    int pc = int(address & 127);
    if (rom.ok()) {
        const int direction = delta < 0 ? -1 : 1;
        for (int i = 0; i < std::abs(delta); ++i) {
            int next = pc + direction;
            while (next >= 0 && next < 128 && patchName(rom, mode, msb, lsb, next).empty()) next += direction;
            if (next < 0 || next >= 128) break;
            pc = next;
        }
    } else pc = std::clamp(pc + delta, 0, 127);
    if (pc != int(address & 127)) {
        selectAddress(part, (address & ~127u) | unsigned(pc));
    }
}

// Opaque per-slot state, saved by Schwung. Sparse records keep common saves small.
constexpr std::string_view stateVersion = "MU2:";
struct PartState {
    unsigned address = 0;
    std::array<unsigned, 3> groups = defaultGroups;
    std::array<int, knobs.size()> values = defaultKnobs;
};
constexpr size_t stateRecord = 2 + 4 * 6 + 2 * knobs.size();
bool fromHex(std::string_view& remaining, size_t width, unsigned maximum, unsigned& value) {
    if (remaining.size() < width) return false;
    const auto end = remaining.data() + width;
    const auto result = std::from_chars(remaining.data(), end, value, 16);
    remaining.remove_prefix(width);
    return result.ec == std::errc{} && result.ptr == end && value <= maximum;
}

void restoreState(Instance& instance, std::string_view state) {
#if !MU2000_PLAYER
    const bool buffered = state.ends_with("~B");
    if (buffered) state.remove_suffix(2);
#endif
    const bool oldVersion = state.starts_with("MU1:");
    const bool automatic = state.starts_with("MUF:");
    const bool manual = state.starts_with("MUM:");
    if (!oldVersion && !automatic && !manual && !state.starts_with(stateVersion)) return;
    state.remove_prefix(stateVersion.size());
    constexpr unsigned maxAddress = (1u << 21) - 1;
    const size_t header = oldVersion ? 2 : 3;
    if (state.size() < header || (state.size() - header) % stateRecord || (state.size() - header) / stateRecord > instance.parts.size()) return;
    std::array<PartState, midiParts * midiPorts> parts{};
    unsigned active;
    if (!fromHex(state, 2, unsigned(parts.size() - 1), active)) return;
    unsigned octave = octaveLimit;
    if (!oldVersion && !fromHex(state, 1, 2 * octaveLimit, octave)) return;
    int previous = -1;
    while (!state.empty()) {
        unsigned index;
        if (!fromHex(state, 2, unsigned(parts.size() - 1), index) || int(index) <= previous) return;
        previous = int(index);
        auto& part = parts[index];
        if (!fromHex(state, 6, maxAddress, part.address)) return;
        for (size_t g = 0; g < part.groups.size(); ++g)
            if (!fromHex(state, 6, maxAddress, part.groups[g]) || groupIndex(part.groups[g]) != g) return;
        if (part.groups[groupIndex(part.address)] != part.address) return;
        for (auto& knob : part.values) {
            unsigned value;
            if (!fromHex(state, 2, 127, value)) return;
            knob = int(value);
        }
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        auto& part = instance.parts[i];
        uint32_t changed = part.address.exchange(parts[i].address) != parts[i].address ? patchBit : 0;
        for (size_t g = 0; g < parts[i].groups.size(); ++g) part.lastGroup[g] = parts[i].groups[g];
        for (size_t k = 0; k < knobs.size(); ++k)
            if (part.values[k].exchange(parts[i].values[k]) != parts[i].values[k]) changed |= 1u << k;
        if (changed) part.dirty.fetch_or(changed);
    }
#if !MU2000_PLAYER
    instance.fastPads = !buffered;
#endif
    instance.followTrack = automatic || (!manual && active == 0);
    instance.part = instance.followTrack && instance.trackChannel >= 0 ? instance.trackChannel.load() : int(active);
    instance.octave = int(octave) - octaveLimit;
}

int saveState(const Instance& instance, char* out, int capacity) {
    std::array<char, stateVersion.size() + 5 + midiParts * midiPorts * stateRecord> buffer{};
#if MU2000_PLAYER
    const auto prefix = stateVersion;
#else
    const std::string_view prefix = instance.followTrack ? "MUF:" : "MUM:";
#endif
    char* next = std::copy(prefix.begin(), prefix.end(), buffer.begin());
    const auto append = [&](unsigned value, size_t width) {
        constexpr std::string_view hex = "0123456789abcdef";
        for (size_t i = width; i > 0; --i) *next++ = hex[(value >> ((i - 1) * 4)) & 15u];
    };
    append(unsigned(instance.part.load()), 2);
    append(unsigned(instance.octave.load() + octaveLimit), 1);
    for (size_t i = 0; i < instance.parts.size(); ++i) {
        const auto& part = instance.parts[i];
        PartState data;
        data.address = part.address.load();
        for (size_t g = 0; g < data.groups.size(); ++g) data.groups[g] = part.lastGroup[g].load();
        data.groups[groupIndex(data.address)] = data.address;
        for (size_t k = 0; k < knobs.size(); ++k) data.values[k] = part.values[k].load();
        if (data.address == 0 && data.groups == defaultGroups && data.values == defaultKnobs) continue;
        append(unsigned(i), 2);
        append(data.address, 6);
        for (unsigned group : data.groups) append(group, 6);
        for (int value : data.values) append(unsigned(value), 2);
    }
#if !MU2000_PLAYER
    if (!instance.fastPads) { *next++ = '~'; *next++ = 'B'; }
#endif
    const auto size = next - buffer.data();
    if (!out || capacity <= size) return -1;
    std::copy_n(buffer.data(), size, out);
    out[size] = 0;
    return int(size);
}

void set(void* object, const char* name, const char* value) {
    if (!name || !value) return;
    auto& instance = *static_cast<Instance*>(object);
    const std::string_view key(name), text(value);
    try {
#if MU2000_PLAYER
        if (key == "playback_mode") {
            if (text != "live" && text != "render") return;
            const std::lock_guard lock(instance.songMutex);
            const auto mode = text == "live" ? PlaybackMode::live : PlaybackMode::render;
            if (savePlaybackMode(mode)) { instance.playbackMode = mode; instance.fileError.clear(); }
            else instance.fileError = "Could not save playback mode.";
            return;
        }
        if (key == "render_first") {
            const std::lock_guard lock(instance.songMutex);
            const auto mode = instance.rendered.load();
            instance.overloaded = false;
            instance.renderFailure = RenderFailure::none;
            if (instance.playing.load() || mode == RenderedMode::playing || mode == RenderedMode::requested) {
                instance.fileError = "Stop playback before rendering.";
            } else if (instance.durationMs.load() == 0 || instance.durationMs.load() >= renderedLimitSeconds * 1000) {
                instance.fileError = "Render First needs a MIDI file shorter than ten minutes.";
            } else {
                instance.fileError.clear();
                instance.positionMs = 0;
                instance.seekMillis = -1;
                instance.rendered = RenderedMode::requested;
            }
            return;
        }
        if (key == "render_live") {
            const auto mode = instance.rendered.load();
            if (mode == RenderedMode::paused || mode == RenderedMode::finished) {
                const std::lock_guard lock(instance.songMutex);
                requestTransportStop(instance);
                instance.rewind = true;
                instance.ended = false;
                instance.rendered = RenderedMode::off;
            }
            return;
        }
        if (key == "loop" || key == "usb") {
            if (text != "0" && text != "1") return;
            if (key == "loop") instance.loop = text == "1";
            else if (instance.usb.exchange(text == "1") && text == "0" && !instance.playing.load())
                instance.stop = true;
            return;
        }
        if (key == "panic") {
            const std::lock_guard lock(instance.songMutex);
            auto expected = RenderedMode::playing;
            instance.rendered.compare_exchange_strong(expected, RenderedMode::paused);
            if (expected == RenderedMode::requested || expected == RenderedMode::preparing)
                instance.rendered = RenderedMode::off;
            requestTransportStop(instance);
            return;
        }
        if (key == "seek") {
            int seconds = 0;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), seconds);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || seconds < 0 || seconds > 86400) return;
            const auto mode = instance.rendered.load();
            if (mode == RenderedMode::playing || mode == RenderedMode::paused || mode == RenderedMode::finished) {
                const auto target = std::min(unsigned(seconds) * 1000, instance.durationMs.load());
                instance.cachedRead = unsigned(uint64_t(target) * sampleRate / (1000 * framesPerBlock));
                instance.positionMs = target;
                if (mode == RenderedMode::finished) instance.rendered = RenderedMode::paused;
            } else if (mode == RenderedMode::off) {
                const std::lock_guard lock(instance.songMutex);
                const auto target = std::min(unsigned(seconds) * 1000, instance.durationMs.load());
                instance.audioGeneration.fetch_add(1, std::memory_order_release);
                instance.heardMs = target;
                instance.seekMillis = int(target);
                if (instance.fileAudible.load()) {
                    instance.playing = true;
                    instance.primingFile = true;
                }
            }
            return;
        }
        const auto colon = key.find(':');
        if (colon != std::string_view::npos) {
            unsigned idx = 0;
            const auto parsed = std::from_chars(key.data() + colon + 1, key.data() + key.size(), idx);
            if (parsed.ec == std::errc{} && parsed.ptr == key.data() + key.size() && idx < instance.mix.size()) {
                auto& mix = instance.mix[idx];
                const auto field = key.substr(0, colon);
                if (field == "mute" || field == "solo") {
                    if (text == "0" || text == "1") (field == "mute" ? mix.mute : mix.solo) = text == "1";
                    return;
                }
                int number = 0;
                const auto val = std::from_chars(text.data(), text.data() + text.size(), number);
                if (field == "trim" && val.ec == std::errc{} && val.ptr == text.data() + text.size() && number >= 0 && number <= maxTrim) {
                    mix.trim = number;
                    mix.changed = true;
                }
            }
            return;
        }
#endif
        if (key == "state") { restoreState(instance, text); return; }
        if (key == "file_path") {
            if (!loadSong(instance, value)) {
                const std::lock_guard lock(instance.songMutex);
                instance.fileError = "Could not open MIDI file.";
            }
            return;
        }
        if (key == "stop") {
#if MU2000_PLAYER
            const std::lock_guard lock(instance.songMutex);
            const auto mode = instance.rendered.load();
            if (mode == RenderedMode::playing) {
                instance.rendered = RenderedMode::paused;
                return;
            }
            if (mode == RenderedMode::preparing || mode == RenderedMode::requested) {
                instance.rendered = RenderedMode::off;
                instance.rewind = true;
            }
            const auto resume = instance.heardMs.load();
            const bool livePause = mode == RenderedMode::off && instance.fileAudible.load();
            requestTransportStop(instance);
            if (livePause) {
                instance.ended = false;
                instance.seekMillis = int(resume);
            }
            return;
#else
            instance.playing = false; instance.stop = true; return;
#endif
        }
        if (key == "play") {
            const std::lock_guard lock(instance.songMutex);
#if MU2000_PLAYER
            const auto mode = instance.rendered.load();
            if (mode != RenderedMode::off) {
                if (mode == RenderedMode::finished) instance.cachedRead = 0;
                if (mode == RenderedMode::paused || mode == RenderedMode::finished) instance.rendered = RenderedMode::playing;
                return;
            }
            if (instance.fileAudible.load()) return;
            instance.renderFailure = RenderFailure::none;
#endif
            if (!instance.durationMs.load()) return;
            if (instance.ended.exchange(false)) {
                instance.rewind = true;
#if MU2000_PLAYER
                instance.heardMs = 0;
#endif
            }
            instance.overloaded = false;
            instance.fileError.clear();
#if MU2000_PLAYER
            instance.audioGeneration.fetch_add(1, std::memory_order_release);
            instance.primingFile = true;
            instance.fileAudible = true;
#endif
            instance.playing = true;
            return;
        }
        int number = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (key == "voice_address") {
            if (error == std::errc{} && end == text.data() + text.size() && number >= 0 && number < (1 << 21))
                selectAddress(instance.parts[size_t(instance.part.load())], unsigned(number));
            return;
        }
        if (error != std::errc{} || number < -127 || number > 127) return;
        const auto fraction = std::string_view(end, text.data() + text.size());
        if (!fraction.empty() && (!fraction.starts_with('.') || fraction.size() == 1 ||
            !std::all_of(fraction.begin() + 1, fraction.end(), [](char c) { return c == '0'; }))) return;
#if !MU2000_PLAYER
        if (key == "fast_pads" && (number == 0 || number == 1)) { instance.fastPads = number == 1; return; }
#endif
        if (key == "octave") {
            if (number >= -octaveLimit && number <= octaveLimit) instance.octave = number;
            return;
        }
        if (key == "track_channel" && number >= -1 && number < midiParts) {
            instance.trackChannel = number;
            if (instance.followTrack && number >= 0) instance.part = number;
            return;
        }
        if (key == "edit_target" && number >= -1 && number < midiPorts * midiParts) {
            instance.followTrack = number < 0;
            if (number >= 0) instance.part = number;
            else if (instance.trackChannel >= 0) instance.part = instance.trackChannel.load();
            return;
        }
        auto& part = instance.parts[size_t(instance.part.load())];
        if (key == "browse") { if (number) browse(instance, part, number); return; }
        if (key == "variation") { if (number) browseVariations(instance, part, number); return; }
        if (key == "group") {
            if (number == 0 || mu_pilot::isKitBank(number))
                selectAddress(part, part.lastGroup[groupIndex(kitAddress(number))].load());
            return;
        }
        for (size_t k = 0; k < knobs.size(); ++k) {
            const bool step = key.starts_with("step:") && key.substr(5) == knobs[k].first;
            if (!step && (key != knobs[k].first || number < 0)) continue;
            part.values[k] = step ? std::clamp(part.values[k].load() + number, 0, 127) : number;
            part.dirty.fetch_or(1u << k);
            return;
        }
        if (number < 0) return;
#if MU2000_PLAYER
        if (key == "master" && number >= 0 && number <= maxTrim) { instance.master = number; return; }
#endif
        if (key == "part" && number < midiPorts * midiParts) { instance.followTrack = false; instance.part = number; return; }
        const int shift = key == "bank" ? 14 : key == "lsb" ? 7 : key == "program" ? 0 : -1;
        if (shift >= 0) {
            const unsigned address = part.address.load();
            selectAddress(part, (address & ~(127u << shift)) | (unsigned(number) << shift));
        }
    } catch (...) {}
}

int reply(std::string_view value, char* out, int capacity) {
    if (!out || capacity < 1) return -1;
    const auto n = std::min(value.size(), size_t(capacity - 1));
    if (n > 0) std::memcpy(out, value.data(), n);
    out[n] = 0;
    return int(n);
}

int get(void* object, const char* name, char* out, int capacity) {
    if (!name) return -1;
    auto& instance = *static_cast<Instance*>(object);
    const std::string_view key(name);
#if MU2000_PLAYER
    const bool liveMode = instance.rendered.load() == RenderedMode::off;
    const auto clockMs = liveMode ? instance.heardMs.load() : instance.positionMs.load();
    if (key == "position_ms") return reply(std::to_string(clockMs), out, capacity);
    if (key == "live_buffer_ms") {
        const auto read = instance.audioRead.load(std::memory_order_acquire);
        const auto queued = std::min(queueCapacity, instance.audioWrite.load(std::memory_order_acquire) - read);
        return reply(std::to_string(liveMode && instance.fileAudible ? uint64_t(queued) * framesPerBlock * 1000 / sampleRate : 0), out, capacity);
    }
    if (key == "playback_mode") return reply(instance.playbackMode == PlaybackMode::live ? "live" : "render", out, capacity);
    if (key == "render_retry") {
        const auto duration = instance.durationMs.load();
        return reply(instance.overloaded.load() && duration > 0 && duration < renderedLimitSeconds * 1000 ? "1" : "0", out, capacity);
    }
    if (key == "transport_info") {
        const auto mode = instance.rendered.load();
        const auto state = mode == RenderedMode::requested || mode == RenderedMode::preparing ? 5 :
            mode == RenderedMode::playing ? 1 : mode == RenderedMode::paused ? 0 :
            mode == RenderedMode::finished ? 2 :
            instance.renderFailure.load() != RenderFailure::none || instance.overloaded.load() ? 4 : instance.seeking.load() ? 3 :
            instance.fileAudible.load() ? 1 : instance.ended.load() ? 2 : 0;
        return reply(std::to_string(state) + "," + std::to_string(clockMs) + "," +
            std::to_string(instance.durationMs.load()) + "," + std::to_string((instance.cpuAvg.load() + 50) / 100) +
            "," + std::to_string((instance.cpuPeak.load() + 50) / 100) + "," + std::to_string(instance.voices.load()) +
            "," + std::to_string(instance.underruns.load()) + "," + std::to_string(instance.midiDrops.load()), out, capacity);
    }
    if (key == "loop") return reply(instance.loop ? "1" : "0", out, capacity);
    if (key == "rendered") {
        const auto mode = instance.rendered.load();
        return reply(mode == RenderedMode::requested || mode == RenderedMode::preparing ? "preparing" :
            mode == RenderedMode::off ? "off" : "cached", out, capacity);
    }
    if (key == "usb") return reply(instance.usb ? "1" : "0", out, capacity);
    if (key == "ended") return reply(instance.ended && !instance.fileAudible ? "1" : "0", out, capacity);
    if (key == "seeking") return reply(instance.seeking ? "1" : "0", out, capacity);
    if (key == "duration_ms") return reply(std::to_string(instance.durationMs.load()), out, capacity);
    if (key == "master") return reply(std::to_string(instance.master.load()), out, capacity);
    if (key == "cpu") return reply(std::to_string((instance.cpuAvg.load() + 50) / 100) + "," +
        std::to_string((instance.cpuPeak.load() + 50) / 100) + "," + std::to_string(instance.voices.load()), out, capacity);
    const auto colon = key.find(':');
    if (colon != std::string_view::npos) {
        unsigned idx = 0;
        const auto parsed = std::from_chars(key.data() + colon + 1, key.data() + key.size(), idx);
        if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || idx >= instance.mix.size()) return -1;
        const auto& part = instance.mix[idx];
        const auto field = key.substr(0, colon);
        if (field == "used") return reply(part.used ? "1" : "0", out, capacity);
        if (field == "mute") return reply(part.mute ? "1" : "0", out, capacity);
        if (field == "solo") return reply(part.solo ? "1" : "0", out, capacity);
        if (field == "trim") return reply(std::to_string(part.trim.load()), out, capacity);
    }
#endif
#if !MU2000_PLAYER
    if (key == "state") return saveState(instance, out, capacity);
#endif
    if (key == "ready") return reply(instance.activated
#if MU2000_ABI_PROBE
                                    || (instance.synchronousProbe && instance.engine.state() == status::ready)
#endif
                                    ? "1" :
                                    instance.engine.state() == status::failed ? "-1" : "0", out, capacity);
    if (key == "playing") {
#if MU2000_PLAYER
        const auto mode = instance.rendered.load();
        if (mode != RenderedMode::off)
            return reply(mode == RenderedMode::requested || mode == RenderedMode::preparing || mode == RenderedMode::playing ? "1" : "0", out, capacity);
        return reply(instance.fileAudible ? "1" : "0", out, capacity);
#else
        return reply(instance.playing ? "1" : "0", out, capacity);
#endif
    }
    if (key == "underruns") return reply(std::to_string(instance.underruns.load()), out, capacity);
    if (key == "clipped_samples") return reply(std::to_string(instance.clipped.load()), out, capacity);
    if (key == "midi_drops") return reply(std::to_string(instance.midiDrops.load()), out, capacity);
#if !MU2000_PLAYER
    if (key == "fast_pads") return reply(instance.fastPads ? "1" : "0", out, capacity);
    if (key == "buffer_frames") return reply(std::to_string((instance.fastPads ? fastPadBlocks : renderAhead) * MOVE_FRAMES_PER_BLOCK), out, capacity);
#else
    if (key == "buffer_frames") return reply(std::to_string(renderAhead * MOVE_FRAMES_PER_BLOCK), out, capacity);
#endif
    if (key == "loaded") { const std::lock_guard lock(instance.songMutex); return reply(instance.song.name, out, capacity); }
    if (key == "status") {
        const std::lock_guard lock(instance.songMutex);
        if (!instance.fileError.empty()) return reply(instance.fileError, out, capacity);
#if MU2000_PLAYER
        if (instance.renderFailure.load() == RenderFailure::memory) return reply("Not enough memory to render this file.", out, capacity);
        if (instance.renderFailure.load() == RenderFailure::capacity) return reply("Render stopped: file exceeded its estimated duration.", out, capacity);
        if (instance.overloaded.load()) return reply("Playback stopped: render overload.", out, capacity);
#endif
        return reply(instance.engine.state() == status::failed ? "MU2000 ROMs not found or invalid." :
                     !instance.activated ? "Starting Muve2000." : "Ready.", out, capacity);
    }
    if (key == "part") return reply(std::to_string(instance.part.load()), out, capacity);
    if (key == "octave") return reply(std::to_string(instance.octave.load()), out, capacity);
    if (key == "editor_instance") return reply("mu" + std::to_string(instance.editorId), out, capacity);
    if (key == "edit_target") return reply(std::to_string(instance.followTrack ? -1 : instance.part.load()), out, capacity);
    const auto& part = instance.parts[size_t(instance.part.load())];
    const auto address = part.address.load();
    if (key.starts_with("patch_rows:") || key.starts_with("variation_rows:")) {
        const bool variations = key.starts_with("variation_rows:");
        const auto offset = key.substr(key.find(':') + 1);
        unsigned start = 0;
        const auto result = std::from_chars(offset.data(), offset.data() + offset.size(), start);
        if (result.ec != std::errc{} || result.ptr != offset.data() + offset.size()) return -1;
        const auto rom = voiceRom();
        const auto choices = variations ? variationAddresses(instance, address) : std::vector<unsigned>{};
        const unsigned size = variations ? unsigned(choices.size()) : 128;
        std::string rows;
        // Small pages fit the host's parameter-response buffer, including long voice names.
        for (unsigned n = start; n < size && n - start < 8; ++n) {
            const auto a = variations ? choices[n] : ((address & ~127u) | n);
            const auto label = patchName(rom, instance.voiceMode.load(), int(a >> 14), int((a >> 7) & 127), int(a & 127));
            if (!label.empty()) rows += std::to_string(variations ? a : n) + "\t" + label + "\n";
        }
        if (start < size && size - start > 8) rows += "+";
        return reply(rows, out, capacity);
    }
    if (key == "bank") return reply(std::to_string(address >> 14), out, capacity);
    if (key == "lsb") return reply(std::to_string((address >> 7) & 127), out, capacity);
    if (key == "program") return reply(std::to_string(address & 127), out, capacity);
    if (key == "patch_name" || key == "preset_name") {
        const auto rom = voiceRom();
        auto label = patchName(rom, instance.voiceMode.load(), int(address >> 14), int((address >> 7) & 127), int(address & 127));
        return reply(label.empty() ? "Program " + std::to_string((address & 127) + 1) : label, out, capacity);
    }
    for (size_t k = 0; k < knobs.size(); ++k)
        if (key == knobs[k].first) return reply(std::to_string(part.values[k].load()), out, capacity);
    return -1;
}

int error(void* object, char* out, int capacity) {
    auto& instance = *static_cast<Instance*>(object);
    if (instance.engine.state() == status::failed) return get(object, "status", out, capacity);
    return reply({}, out, capacity);
}

plugin_api_v2_t api{MOVE_PLUGIN_API_VERSION_2, create,
    [](void* object) { delete static_cast<Instance*>(object); },
    incoming, set, get, error, render};
}

extern "C" __attribute__((visibility("default"))) plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t* host) {
    return host && host->sample_rate == sampleRate ? &api : nullptr;
}
