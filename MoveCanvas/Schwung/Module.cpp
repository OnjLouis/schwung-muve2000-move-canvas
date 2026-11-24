#include "Engine.h"
#include "../Source/ProgramCatalogue.h"
#include "plugin_api_v1.h"
#include "smf.h"
#include <array>
#include <charconv>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace {
using namespace scbridge;
constexpr unsigned hostFrames = MOVE_FRAMES_PER_BLOCK, quantum = 2 * hostFrames;
constexpr unsigned fastPadBlocks = 5;
constexpr unsigned queueBlocks = 8, midiCapacity = 131072;
constexpr unsigned rate = MOVE_SAMPLE_RATE;
using Audio = std::array<int16_t, hostFrames * 2>;
using Values = std::array<int32_t, parameterCount>;
using Generations = std::array<uint32_t, parameterCount>;
constexpr auto library = "/data/UserData/UserLibrary";
const host_api_v1_t *hostApi = nullptr;

int number(std::string_view s, int fallback = -1) {
    int n;
    const auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), n);
    return err == std::errc{} && end == s.data() + s.size() ? n : fallback;
}
bool append(Shared &packet, const uint8_t *data, size_t size, unsigned offset = 0) {
    if (!size || size > maxMidiBytes - packet.midiBytes || packet.eventCount >= maxEvents)
        return false;
    packet.events[packet.eventCount++] = {offset, unsigned(size), packet.midiBytes};
    std::memcpy(packet.midi + packet.midiBytes, data, size);
    packet.midiBytes += unsigned(size);
    return true;
}
void silence(Shared &p) {
    for (unsigned ch = 0; ch < 16; ++ch)
        for (unsigned cc : {64u, 66u, 120u, 123u}) {
            uint8_t m[]{uint8_t(0xb0 | ch), uint8_t(cc), 0};
            append(p, m, 3);
        }
}
struct MidiStream {
    std::array<std::array<uint8_t, 128>, 16> notes{};
    std::atomic<unsigned> midiRead{0}, midiWrite{0};
    std::array<uint8_t, midiCapacity> midi{};
    // One producer per host MIDI dispatcher. Each complete callback is published at once.
    std::array<uint8_t, maxMidiBytes> assembling{};
    unsigned filled = 0, needed = 0;
    uint8_t running = 0;
    bool input(const uint8_t *data, unsigned size) {
        auto write = midiWrite.load(std::memory_order_relaxed);
        if (size > midiCapacity - (write - midiRead.load(std::memory_order_acquire)))
            return false;
        for (unsigned i = 0; i < size; ++i)
            midi[(write + i) % midiCapacity] = data[i];
        midiWrite.store(write + size, std::memory_order_release);
        return true;
    }
    void drain(Shared &p, int route, std::atomic<unsigned> &drops) {
        unsigned read = midiRead.load(std::memory_order_relaxed);
        const unsigned end = midiWrite.load(std::memory_order_acquire);
        while (read != end && p.eventCount < maxEvents && p.midiBytes + maxMidiBytes / 2 < maxMidiBytes) {
            const uint8_t b = midi[read++ % midiCapacity];
            if (b >= 0xf8)
                continue;
            if (b == 0xf7 && filled && assembling[0] == 0xf0) {
                if (filled < assembling.size()) {
                    assembling[filled++] = b;
                    if (!append(p, assembling.data(), filled))
                        ++drops;
                }
                filled = needed = 0;
                continue;
            }
            if (b & 0x80) {
                filled = needed = 0;
                running = 0;
                if (b == 0xf0) {
                    assembling[filled++] = b;
                    continue;
                }
                if (b >= 0xf0)
                    continue;
                running = b;
                needed = ((b & 0xf0) == 0xc0 || (b & 0xf0) == 0xd0) ? 2 : 3;
                assembling[filled++] = b;
                continue;
            }
            if (!filled && running)
                assembling[filled++] = running;
            if (!filled)
                continue;
            if (filled == assembling.size()) {
                ++drops;
                filled = needed = 0;
                continue;
            }
            assembling[filled++] = b;
            if (needed && filled == needed) {
                uint8_t message[]{assembling[0], assembling[1], assembling[2]};
                const auto command = message[0] & 0xf0;
                const auto ch = message[0] & 15;
                int dest = route >= 0 ? route : ch;
                if (command == 0x90 || command == 0x80) {
                    auto &saved = notes[ch][message[1]];
                    if (command == 0x90 && message[2])
                        saved = uint8_t(dest + 1);
                    else if (saved) {
                        dest = saved - 1;
                        saved = 0;
                    }
                }
                message[0] = uint8_t(command | dest);
                if (!append(p, message, filled))
                    ++drops;
                filled = 0;
            }
        }
        midiRead.store(read, std::memory_order_release);
    }
};

struct Instance {
    std::filesystem::path runtime;
    bool tool;
    inline static std::atomic<unsigned> nextEditorId{1};
    const unsigned editorId = nextEditorId.fetch_add(1);
    bool followTrack = true, browsingKit = false;
    int trackChannel = -1, melodicPart = 0;
    std::atomic<bool> fastPads{true};
    std::atomic<int> incomingChannel{-1};
    std::atomic<bool> quit{false}, ready{false}, active{false}, usb{true}, loop{false};
    std::atomic<unsigned> audioRead{0}, audioWrite{0}, gaps{0}, drops{0};
    std::array<Audio, queueBlocks> audio{};
    MidiStream internal, external;
    std::mutex model;
    Values values{};
    Generations changes{};
    std::optional<EngineScene> scene, restore;
    std::string status = "Starting engine";
    std::shared_ptr<const smf::Song> pendingSong;
    std::string filename, pendingPath;
    std::atomic<bool> playing{false}, panic{false}, renderFirst{true}, preparing{false};
    std::atomic<unsigned> renderPercent{0}, ports{1};
    std::atomic<bool> follow{false};
    std::atomic<bool> settingsDirty{false};
    std::atomic<int> clockStatus{MOVE_CLOCK_STATUS_UNAVAILABLE};
    std::atomic<double> beat{-1}, tempo{120};
    std::atomic<unsigned> locked{0}, transportRevision{0};
    std::atomic<int> seekMs{-1};
    std::atomic<unsigned> posMs{0}, lengthMs{0}, mute{0}, solo{0};
    std::array<std::atomic<int>, 16> trims{};
    std::atomic<unsigned> mixRevision{0};
    std::thread worker;
    Instance(const std::filesystem::path &path, bool player) : runtime(path), tool(player) {
        for (unsigned i = 0; i < parameterCount; ++i)
            values[i] = controls[i].initial;
        if (!tool) values[15] = 1; // A keyboard plays the part being edited, including drum kits.
        for (auto &trim : trims)
            trim.store(100);
        if (tool) {
            int version, r, f, u, l;
            std::ifstream settings(settingsPath());
            if (settings >> version >> r >> f >> u >> l && version == 1) {
                renderFirst = r == 1;
                follow = !renderFirst && f == 1;
                usb = u == 1;
                loop = l == 1;
            }
        }
        worker = std::thread([this] {
            try {
                run();
            } catch (...) {
                setStatus("Engine worker failed");
            }
            ready = false;
        });
    }
    ~Instance() {
        quit = true;
        worker.join();
        saveSettings();
    }
    std::filesystem::path settingsPath() const {
        return runtime.parent_path().parent_path().parent_path().parent_path() / "config" /
               "sound-canvas-player.settings";
    }
    void saveSettings() {
        if (!settingsDirty.exchange(false) || !tool)
            return;
        auto path = settingsPath(), tmp = path;
        tmp += ".tmp";
        std::ofstream out(tmp);
        out << "1 " << renderFirst.load() << " " << follow.load() << " " << usb.load() << " " << loop.load()
            << "\n";
        out.close();
        std::error_code error;
        if (out)
            std::filesystem::rename(tmp, path, error);
        if (!out || error)
            setStatus("Could not save player preferences");
    }
    void setStatus(std::string value) {
        std::lock_guard lock(model);
        status = std::move(value);
    }
    void run();
    bool prepare(move::Engine &, const smf::Song &, std::vector<Audio> &);
};

bool Instance::prepare(move::Engine &engine, const smf::Song &song, std::vector<Audio> &cache) {
    if (song.endSec > 600) {
        setStatus("Render First supports ten minutes. Choose Live for longer files.");
        return false;
    }
    auto &p = engine.packet;
    constexpr unsigned batch = 1024;
    const uint64_t frames = uint64_t((song.endSec + 2) * rate);
    cache.clear();
    try {
        cache.resize((frames + batch - 1) / batch * (batch / hostFrames));
    } catch (const std::bad_alloc &) {
        setStatus("Insufficient memory to render this file");
        return false;
    }
    const auto mask = ports.load();
    const unsigned passes = std::popcount(mask);
    const auto revision = transportRevision.load();
    const auto muted = mute.load(), soloed = solo.load(), locks = locked.load();
    auto volume = [&](unsigned ch, int v) {
        return uint8_t(((muted >> ch) & 1 || (soloed && !(soloed & (1u << ch)))) ? 0 : v * trims[ch].load() / 100);
    };
    unsigned pass = 0;
    for (unsigned port = 0; port < 4; ++port) {
        if (!(mask & (1u << port)))
            continue;
        p.frames = batch;
        p.reset = 1;
        p.parameters[15] = 0;
        p.eventCount = p.midiBytes = p.restoreSceneValid = 0;
        if (!engine.render('S')) {
            setStatus("Cannot initialize file render");
            return false;
        }
        p.reset = 0;
        size_t next = 0;
        for (uint64_t begin = 0; begin < frames; begin += batch) {
            if (quit || !playing || !renderFirst || transportRevision.load() != revision) {
                setStatus("Render cancelled");
                return false;
            }
            p.eventCount = p.midiBytes = 0;
            if (!begin)
                for (unsigned ch = 0; ch < 16; ++ch) {
                    uint8_t cc[]{uint8_t(0xb0 | ch), 7, volume(ch, 100)};
                    append(p, cc, 3);
                }
            while (next < song.events.size()) {
                const auto &e = song.events[next];
                const auto frame = uint64_t(e.sec * rate);
                if (frame >= begin + batch)
                    break;
                if (e.port != port) {
                    ++next;
                    continue;
                }
                const auto *data = e.m;
                size_t size = e.len;
                if (e.sysex >= 0) {
                    const auto &syx = song.sysex.at(size_t(e.sysex));
                    data = syx.data();
                    size = syx.size();
                }
                const unsigned ch = data[0] & 15;
                if ((locks & (1u << ch)) &&
                    ((data[0] & 0xf0) == 0xc0 || ((data[0] & 0xf0) == 0xb0 && (data[1] == 0 || data[1] == 32)))) {
                    ++next;
                    continue;
                }
                uint8_t cc[]{data[0], 7, 0};
                if (size == 3 && (data[0] & 0xf0) == 0xb0 && data[1] == 7) {
                    cc[2] = volume(ch, data[2]);
                    data = cc;
                }
                if (!append(p, data, size, unsigned(frame - begin))) {
                    setStatus("MIDI exceeds engine block capacity");
                    return false;
                }
                ++next;
            }
            if (!engine.render('P')) {
                setStatus("File render failed");
                return false;
            }
            for (unsigned f = 0; f < batch; f += hostFrames) {
                auto &out = cache[(begin + f) / hostFrames];
                for (unsigned j = 0; j < hostFrames; ++j) {
                    out[2 * j] =
                        int16_t(std::clamp(out[2 * j] + p.left[f + j] * (32767.f / passes), -32768.f, 32767.f));
                    out[2 * j + 1] = int16_t(
                        std::clamp(out[2 * j + 1] + p.right[f + j] * (32767.f / passes), -32768.f, 32767.f));
                }
            }
            renderPercent = std::min(100u, unsigned((100 * pass + 100 * (begin + batch) / frames) / passes));
        }
        ++pass;
    }
    return !cache.empty();
}

void loadSong(Instance &i, const std::string &value) {
    try {
        const auto path = std::filesystem::canonical(value);
        if (!path.generic_string().starts_with(std::string(library) + "/") ||
            std::filesystem::file_size(path) > 16 * 1024 * 1024)
            throw std::runtime_error("Unsupported file");
        auto song = std::make_shared<smf::Song>(smf::load(path.string()));
        if (song->endSec < 0 || song->endSec > 86400)
            throw std::runtime_error("Unsupported length");
        unsigned used = 0;
        if (!song->warnings.empty())
            throw std::runtime_error(song->warnings.front());
        for (const auto &e : song->events) {
            if (e.port >= 4)
                throw std::runtime_error("File uses a port outside A to D");
            used |= 1u << e.port;
        }
        if (!used)
            throw std::runtime_error("Empty MIDI file");
        std::lock_guard lock(i.model);
        if (!i.pendingPath.empty())
            return;
        i.panic = true;
        i.seekMs = -1;
        i.lengthMs = unsigned(song->endSec * 1000);
        i.ports = used;
        if (std::popcount(used) > 1) {
            i.renderFirst = true;
            i.follow = false;
        }
        i.filename = path.filename().string();
        i.pendingSong = std::move(song);
        i.status = "Loaded " + i.filename;
    } catch (const std::exception &error) {
        i.setStatus(std::string("MIDI file error: ") + error.what());
    }
}

void Instance::run() {
    move::Engine engine(quit);
    if (!engine.start(runtime)) {
        setStatus(engine.startError);
        return;
    }
    auto &p = engine.packet;
    p.frames = quantum;
    p.reset = 1;
    if (!engine.render('S') || !p.advancedReady) {
        setStatus("Unsupported engine or failed initialization");
        return;
    }
    p.reset = 0;
    setStatus("Ready");
    ready = true;
    std::shared_ptr<const smf::Song> song;
    uint64_t position = 0;
    size_t event = 0;
    std::array<int, 16> levels;
    levels.fill(100);
    unsigned lastMix = 0, silentBlocks = 0, snapshotBlock = 0;
    Generations applied{};
    std::vector<Audio> cache;
    size_t cachedBlock = 0;
    unsigned cachedMix = 0;
    Generations cachedControls{};
    bool clockWasRunning = false;
    double anchor = 0;
    while (!quit.load()) {
        saveSettings();
        const auto write = audioWrite.load();
        const bool fast = !tool && fastPads.load(std::memory_order_relaxed);
        const auto depth = fast ? fastPadBlocks : queueBlocks;
        if (write - audioRead.load(std::memory_order_acquire) > depth - quantum / hostFrames) {
            std::this_thread::sleep_for(std::chrono::microseconds(fast ? 250 : 1000));
            continue;
        }
        std::string path;
        {
            std::lock_guard lock(model);
            path.swap(pendingPath);
        }
        if (!path.empty())
            loadSong(*this, path);
        p.eventCount = p.midiBytes = 0;
        p.restoreSceneValid = p.reset = 0;
        {
            std::lock_guard lock(model);
            if (pendingSong) {
                song = std::move(pendingSong);
                position = event = 0;
                posMs = 0;
                cache.clear();
                cachedBlock = 0;
                p.reset = 1;
                levels.fill(100);
            }
            if (restore) {
                p.restoredScene = *restore;
                p.restoreSceneValid = p.reset = 1;
                restore.reset();
            }
            const int heard = incomingChannel.exchange(-1);
            if (!tool && followTrack && !browsingKit && trackChannel < 0 && heard >= 0 && values[0] != heard) {
                values[0] = heard;
                ++changes[0];
            }
            std::copy(values.begin(), values.end(), p.parameters);
            std::copy(changes.begin(), changes.end(), p.generations);
        }
        const bool syncing = song && follow && !renderFirst;
        const bool clockRunning = syncing && clockStatus == MOVE_CLOCK_STATUS_RUNNING && beat >= 0;
        const bool clockStarted = clockRunning && !clockWasRunning;
        if (clockRunning != clockWasRunning) {
            playing = clockRunning;
            if (clockRunning) {
                anchor = std::ceil(beat.load() / 4.0) * 4.0;
                seekMs = 0;
                setStatus("Following Move");
            } else {
                panic = true;
                setStatus("Paused");
            }
            clockWasRunning = clockRunning;
        }
        if (!cache.empty() && (cachedMix != mixRevision.load() ||
                               !std::equal(cachedControls.begin(), cachedControls.end(), p.generations))) {
            cache.clear();
            cachedBlock = 0;
        }
        if (tool && song && renderFirst && playing) {
            if (cache.empty()) {
                preparing = true;
                active = false;
                renderPercent = 0;
                setStatus("Preparing MIDI file");
                const bool ok = prepare(engine, *song, cache);
                cachedMix = mixRevision.load();
                std::copy(std::begin(p.generations), std::end(p.generations), cachedControls.begin());
                p.frames = quantum;
                p.eventCount = p.midiBytes = 0;
                p.reset = 0;
                preparing = false;
                if (!ok) {
                    cache.clear();
                    playing = false;
                    panic = true;
                    continue;
                }
                setStatus("Playing rendered MIDI");
            }
            const int seek = seekMs.exchange(-1);
            if (seek >= 0)
                cachedBlock = std::min(cache.size() - 1, size_t(seek) * rate / (1000 * hostFrames));
            for (unsigned count = 0; count < quantum / hostFrames; ++count) {
                if (cachedBlock == cache.size()) {
                    cachedBlock = 0;
                    if (!loop.load()) {
                        playing = false;
                        posMs = 0;
                        setStatus("End of file");
                        break;
                    }
                }
                audio[(write + count) % queueBlocks] = cache[cachedBlock++];
                audioWrite.store(write + count + 1, std::memory_order_release);
                active = true;
                posMs = unsigned(cachedBlock * hostFrames * 1000 / rate);
            }
            continue;
        }
        if (!renderFirst && !cache.empty()) {
            std::vector<Audio>().swap(cache);
            cachedBlock = 0;
        }
        if (panic.exchange(false))
            silence(p);
        int targetMs = seekMs.exchange(-1);
        if (song && targetMs >= 0) {
            cachedBlock = size_t(std::min(unsigned(targetMs), lengthMs.load())) * rate / (1000 * hostFrames);
            if (renderFirst && !playing) {
                posMs = unsigned(targetMs);
                targetMs = -1;
            }
        }
        if (song && targetMs >= 0) {
            position = uint64_t(std::min(unsigned(targetMs), lengthMs.load())) * rate / 1000;
            if (clockRunning && !clockStarted)
                anchor = beat.load() - song->qnOf(song->tickAtSec(double(position) / rate));
            event = 0;
            p.reset = 1;
            levels.fill(100);
        }
        const bool resetting = p.reset != 0;
        if (p.reset) {
            active = false;
            if (!engine.render('S')) {
                setStatus("Engine reset failed");
                return;
            }
            p.reset = p.restoreSceneValid = 0;
            p.eventCount = p.midiBytes = 0;
        }
        const bool routeExternal = p.parameters[15] != 0;
        p.parameters[15] = 0; // File channels are independent of live keyboard routing.
        internal.drain(p, p.parameters[0], drops);
        external.drain(p, routeExternal ? p.parameters[0] : -1, drops);
        const auto mix = mixRevision.load();
        const unsigned muted = mute.load(), soloed = solo.load();
        auto volume = [&](unsigned ch) {
            return uint8_t(((muted >> ch) & 1 || (soloed && !(soloed & (1u << ch))))
                               ? 0
                               : levels[ch] * trims[ch].load() / 100);
        };
        if (tool && (lastMix != mix || targetMs >= 0)) {
            for (unsigned ch = 0; ch < 16; ++ch) {
                uint8_t cc[]{uint8_t(0xb0 | ch), 7, volume(ch)};
                append(p, cc, 3);
            }
            lastMix = mix;
        }
        const double bpm = std::clamp(tempo.load(), 20.0, 999.0);
        const double qStart =
            beat.load() - anchor + double(write - audioRead.load()) * hostFrames * bpm / (60 * rate);
        const bool play = song && playing.load() && (!syncing || (clockRunning && qStart >= 0));
        if (song && (play || targetMs >= 0)) {
            // Chase setup messages in order, retaining SysEx and bank/program pairs.
            const uint64_t end = position + (play ? quantum : 0);
            while (event < song->events.size()) {
                const auto &e = song->events[event];
                const auto frame = uint64_t(e.sec * rate);
                unsigned offset = 0;
                if (syncing && play) {
                    const double delta = (e.qn - qStart) * 60.0 * rate / bpm;
                    if (delta >= quantum)
                        break;
                    offset = unsigned(std::max(0.0, delta));
                } else {
                    if (frame >= end)
                        break;
                    offset = unsigned(frame > position ? frame - position : 0);
                }
                const bool chase = targetMs >= 0 && frame < position;
                const auto *data = e.m;
                size_t size = e.len;
                if (e.sysex >= 0) {
                    const auto &syx = song->sysex.at(size_t(e.sysex));
                    data = syx.data();
                    size = syx.size();
                }
                if ((locked.load() & (1u << (data[0] & 15))) &&
                    ((data[0] & 0xf0) == 0xc0 || ((data[0] & 0xf0) == 0xb0 && (data[1] == 0 || data[1] == 32)))) {
                    ++event;
                    continue;
                }
                if (chase && (data[0] & 0xe0) == 0x80) {
                    ++event;
                    continue;
                }
                uint8_t cc[3]{};
                if (size == 3 && (data[0] & 0xf0) == 0xb0 && data[1] == 7) {
                    const unsigned ch = data[0] & 15;
                    levels[ch] = data[2];
                    cc[0] = data[0];
                    cc[1] = 7;
                    cc[2] = volume(ch);
                    data = cc;
                }
                if (!append(p, data, size, offset)) {
                    if (chase && size <= maxMidiBytes && p.eventCount) {
                        if (!engine.render('P')) {
                            setStatus("Seek failed");
                            return;
                        }
                        p.eventCount = p.midiBytes = 0;
                        continue;
                    }
                    ++drops;
                    playing = false;
                    panic = true;
                    setStatus("MIDI exceeds engine block capacity");
                    break;
                }
                ++event;
            }
            if (play) {
                if (syncing)
                    position = uint64_t(song->secAt(song->tickOfQn(qStart + quantum * bpm / (60 * rate))) * rate);
                else
                    position += quantum;
                posMs = unsigned(position * 1000 / rate);
                if (position >= uint64_t(song->endSec * rate) + quantum) {
                    playing = loop.load();
                    seekMs = 0;
                    panic = true;
                    if (syncing && playing)
                        anchor += std::max(4.0, std::ceil(song->endQn / 4.0) * 4.0);
                }
            }
        }
        const bool edited = !std::equal(std::begin(p.generations), std::end(p.generations), applied.begin());
        const bool waking = p.eventCount || edited || play || targetMs >= 0 || resetting;
        const bool setupMidi = fast && std::any_of(p.events, p.events + p.eventCount, [&](const Event &e) {
            const auto type = p.midi[e.position] & 0xf0;
            return type != 0x80 && type != 0x90 && type != 0xa0 && type != 0xd0 && type != 0xe0;
        });
        p.stateEpoch = edited || resetting || setupMidi || (++snapshotBlock % (fast ? 32u : 8u) == 1);
        const bool notesOnly = fast && !p.stateEpoch;
        if (waking)
            silentBlocks = 0;
        if (silentBlocks >= rate / quantum && !waking) {
            active = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        std::copy(std::begin(p.generations), std::end(p.generations), applied.begin());
        if (!engine.render('P', notesOnly)) {
            setStatus("Engine stopped responding");
            return;
        }
        {
            std::lock_guard lock(model);
            if (changes == applied) {
                std::copy(std::begin(p.reported), std::end(p.reported), values.begin());
                values[15] = routeExternal;
                if (p.capturedSceneValid)
                    scene = p.capturedScene;
            }
        }
        int peak = 0;
        for (unsigned f = 0; f < quantum; ++f) {
            auto &block = audio[(write + f / hostFrames) % queueBlocks];
            const auto idx = (f % hostFrames) * 2;
            auto sample = [](float value) { return int16_t(std::clamp(value * 32767.f, -32768.f, 32767.f)); };
            block[idx] = sample(p.left[f]);
            block[idx + 1] = sample(p.right[f]);
            peak = std::max({peak, std::abs(int(block[idx])), std::abs(int(block[idx + 1]))});
        }
        silentBlocks = peak > 1 || waking ? 0 : silentBlocks + 1;
        audioWrite.store(write + quantum / hostFrames, std::memory_order_release);
        active = true;
    }
}

std::string state(const Instance &i) {
    if (!i.scene)
        return {};
    std::string result = i.tool ? "SC2" : i.followTrack ? (i.browsingKit ? "SCK" : "SCF") : i.browsingKit ? std::string("SK") + "0123456789ABCDEF"[i.melodicPart] : "SCM";
    constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    uint32_t bits = 0;
    int pending = 0;
    auto put = [&](unsigned byte) {
        bits = (bits << 8) | byte;
        pending += 8;
        while (pending >= 6) {
            pending -= 6;
            result += digits[(bits >> pending) & 63];
        }
    };
    for (auto value : i.values)
        put(unsigned(value));
    const auto *data = reinterpret_cast<const unsigned char *>(&*i.scene);
    for (size_t n = 0; n < sizeof(EngineScene); ++n)
        put(data[n]);
    if (pending)
        result += digits[(bits << (6 - pending)) & 63];
    if (!i.tool && !i.fastPads) result += "~B";
    return result;
}
void recall(Instance &i, std::string_view text) {
    std::array<uint8_t, parameterCount + sizeof(EngineScene)> data{};
    const bool buffered = text.ends_with("~B");
    if (buffered) text.remove_suffix(2);
    const auto prefix = text.substr(0, 3);
    constexpr std::string_view hex = "0123456789ABCDEF";
    const auto kitPart = prefix.size() == 3 && prefix.starts_with("SK") ? hex.find(prefix[2]) : hex.npos;
    if (text.size() != 3 + (data.size() * 8 + 5) / 6 ||
        (prefix != "SC2" && prefix != "SCF" && prefix != "SCM" && prefix != "SCK" && kitPart == hex.npos))
        return;
    constexpr std::string_view digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    uint32_t bits = 0;
    int pending = 0;
    size_t written = 0;
    for (char c : text.substr(3)) {
        const auto v = digits.find(c);
        if (v == std::string_view::npos)
            return;
        bits = (bits << 6) | uint32_t(v);
        pending += 6;
        if (pending >= 8) {
            pending -= 8;
            data[written++] = uint8_t(bits >> pending);
        }
    }
    if (written != data.size() || (bits & ((1u << pending) - 1)))
        return;
    EngineScene scene;
    std::memcpy(&scene, data.data() + parameterCount, sizeof(scene));
    if (!validScene(scene))
        return;
    for (unsigned n = 0; n < parameterCount; ++n)
        if (data[n] < controls[n].minimum || data[n] > controls[n].maximum)
            return;
    for (unsigned n = 0; n < parameterCount; ++n) {
        i.values[n] = data[n];
        ++i.changes[n];
    }
    i.followTrack = prefix == "SCF" || prefix == "SCK" || (prefix == "SC2" && data[0] == 0);
    i.browsingKit = prefix == "SCK" || kitPart != hex.npos;
    i.melodicPart = kitPart != hex.npos ? int(kitPart) : data[0];
    if (!i.tool && i.followTrack && !i.browsingKit && i.trackChannel >= 0)
        i.values[0] = i.trackChannel;
    i.fastPads = !buffered;
    i.scene = scene; // Keep a valid saved scene available while the worker restores it.
    i.restore = scene;
}

void set(void *object, const char *key, const char *value) {
    if (!object || !key || !value)
        return;
    auto &i = *static_cast<Instance *>(object);
    std::string_view k(key);
    int v = number(value, -128);
    if (k == "file_path" && i.tool) {
        if (std::string_view(value) == "__test_bus__")
            return;
        std::lock_guard lock(i.model);
        i.playing = false;
        ++i.transportRevision;
        i.panic = true;
        i.pendingPath = value;
        i.status = "Opening MIDI file";
        return;
    }
    if (k == "play") {
        i.playing = true;
        i.setStatus("Playing");
        return;
    }
    if (k == "stop") {
        ++i.transportRevision;
        i.playing = false;
        i.panic = true;
        i.setStatus("Paused");
        return;
    }
    if (k == "seek") {
        i.seekMs = std::clamp(v, 0, int(i.lengthMs.load()));
        return;
    }
    if (k == "panic") {
        if (i.tool && i.renderFirst && i.playing) {
            ++i.transportRevision;
            i.playing = false;
            i.setStatus("Paused");
        }
        i.panic = true;
        return;
    }
    if (k == "loop") {
        i.loop = v == 1;
        i.settingsDirty = true;
        return;
    }
    if (k == "usb") {
        i.usb = v == 1;
        i.settingsDirty = true;
        return;
    }
    if (k == "follow") {
        if (v && std::popcount(i.ports.load()) > 1) {
            i.setStatus("Follow Move needs a single-port file");
            return;
        }
        i.follow = v == 1;
        if (v)
            i.renderFirst = false;
        i.playing = false;
        i.panic = true;
        i.seekMs = 0;
        i.settingsDirty = true;
        return;
    }
    if (k == "part_lock") {
        std::lock_guard lock(i.model);
        const auto bit = 1u << i.values[0];
        if (v == 1)
            i.locked.fetch_or(bit);
        else
            i.locked.fetch_and(~bit);
        ++i.mixRevision;
        return;
    }
    if (k == "render_first") {
        if (v == 0 && std::popcount(i.ports.load()) > 1) {
            i.setStatus("Multiport file uses Render First");
            return;
        }
        i.playing = false;
        i.panic = true;
        i.seekMs = 0;
        i.renderFirst = v == 1;
        if (v)
            i.follow = false;
        i.settingsDirty = true;
        return;
    }
    for (auto prefix : {"mute:", "solo:", "trim:"})
        if (k.starts_with(prefix)) {
            int ch = number(k.substr(5));
            if (ch < 0 || ch >= 16)
                return;
            if (k.starts_with("trim:"))
                i.trims[ch] = std::clamp(v, 0, 100);
            else {
                auto &field = k.starts_with("mute:") ? i.mute : i.solo;
                if (v == 1)
                    field.fetch_or(1u << ch);
                else
                    field.fetch_and(~(1u << ch));
            }
            ++i.mixRevision;
            return;
        }
    std::lock_guard lock(i.model);
    if (k == "state") {
        recall(i, value);
        return;
    }
    auto change = [&](unsigned n, int x) {
        i.values[n] = x;
        ++i.changes[n];
    };
    if (!i.tool && k == "fast_pads" && (v == 0 || v == 1)) {
        i.fastPads = v == 1;
        return;
    }
    if (!i.tool && k == "track_channel" && v >= -1 && v < 16) {
        i.trackChannel = v;
        if (i.followTrack && !i.browsingKit && v >= 0 && i.values[0] != v) change(0, v);
        return;
    }
    if (!i.tool && k == "edit_target" && v >= -1 && v < 16) {
        i.followTrack = v < 0;
        i.browsingKit = false;
        if (v >= 0 || i.trackChannel >= 0) change(0, v >= 0 ? v : i.trackChannel);
        return;
    }
    if (k == "preset" && v >= 0 && size_t(v) < catalogueCount) {
        const auto &entry = programCatalogue[v];
        if (!i.browsingKit) i.melodicPart = i.values[0];
        i.browsingKit = entry.drums;
        if (entry.drums)
            change(0, 9);
        else if (!i.tool && i.followTrack && i.trackChannel >= 0)
            change(0, i.trackChannel);
        else if (i.values[0] == 9)
            change(0, i.tool ? 0 : i.melodicPart);
        change(1, entry.map);
        change(2, entry.bank);
        change(3, entry.program);
        if (i.tool)
            i.locked.fetch_or(1u << i.values[0]);
        return;
    }
    for (unsigned n = 0; n < parameterCount; ++n)
        if (k == controls[n].id && v >= controls[n].minimum && v <= controls[n].maximum) {
            if (!i.tool && n == 0) { i.followTrack = false; i.browsingKit = false; }
            change(n, v);
            return;
        }
}

int get(void *object, const char *key, char *out, int capacity) {
    if (!object || !key || !out || capacity <= 0)
        return -1;
    auto &i = *static_cast<Instance *>(object);
    std::lock_guard lock(i.model);
    const std::string_view k(key);
    std::string value;
    if (k == "status")
        value = i.status;
    else if (!i.tool && k == "fast_pads")
        value = i.fastPads ? "1" : "0";
    else if (k == "editor_instance")
        value = "sc" + std::to_string(i.editorId);
    else if (k == "edit_target")
        value = std::to_string(i.followTrack ? -1 : i.browsingKit ? i.melodicPart : i.values[0]);
    else if (k == "module_id")
        value = i.tool ? "sound-canvas-midi-player" : "sound-canvas-bridge";
    else if (k == "part_lock")
        value = (i.locked.load() & (1u << i.values[0])) ? "1" : "0";
    else if (k == "follow")
        value = i.follow ? "1" : "0";
    else if (k == "state")
        value = state(i);
    else if (k == "ready")
        value = i.ready ? "1" : "0";
    else if (k == "preset_count")
        value = std::to_string(catalogueCount);
    else if (k == "preset_name" || k == "preset") {
        const auto *entry = findProgram(i.values[1], i.values[2], i.values[3],
                                        i.scene ? i.scene->partModes[i.values[0]] != 0 : i.values[0] == 9);
        if (k == "preset")
            value = std::to_string(entry ? entry - programCatalogue : -1);
        else
            value = entry ? entry->name : "Unlisted program";
    } else if (k == "playing")
        value = i.playing ? "1" : "0";
    else if (k == "render_first")
        value = i.renderFirst ? "1" : "0";
    else if (k == "preparing")
        value = i.preparing ? "1" : "0";
    else if (k == "render_percent")
        value = std::to_string(i.renderPercent.load());
    else if (k == "loop")
        value = i.loop ? "1" : "0";
    else if (k == "usb")
        value = i.usb ? "1" : "0";
    else if (k == "loaded")
        value = i.filename;
    else if (k == "position_ms")
        value = std::to_string(i.posMs.load());
    else if (k == "duration_ms")
        value = std::to_string(i.lengthMs.load());
    else if (k == "underruns")
        value = std::to_string(i.gaps.load());
    else if (k == "midi_drops")
        value = std::to_string(i.drops.load());
    else if (k.starts_with("mute:") || k.starts_with("solo:") || k.starts_with("trim:")) {
        int ch = number(k.substr(5));
        if (ch < 0 || ch >= 16)
            return -1;
        value = std::to_string(k.starts_with("trim:")
                                   ? i.trims[ch].load()
                                   : int(((k.starts_with("mute:") ? i.mute : i.solo).load() >> ch) & 1));
    } else {
        for (unsigned n = 0; n < parameterCount; ++n)
            if (k == controls[n].id) {
                value = std::to_string(i.values[n]);
                break;
            }
        if (value.empty())
            return -1;
    }
    if (value.size() >= size_t(capacity))
        return -1;
    std::memcpy(out, value.c_str(), value.size() + 1);
    return int(value.size());
}
void *create(const char *dir, const char *) {
    try {
        auto path = std::filesystem::absolute(dir);
        if (path.filename().empty())
            path = path.parent_path();
        bool tool = path.filename() == "sound-canvas-midi-player";
        return new Instance(tool ? path.parent_path().parent_path() / "sound_generators" / "sound-canvas-bridge" /
                                       "runtime"
                                 : path / "runtime",
                            tool);
    } catch (...) {
        return nullptr;
    }
}
void midi(void *object, const uint8_t *data, int count, int source) {
    auto &i = *static_cast<Instance *>(object);
    if (!data || count <= 0 || source == MOVE_MIDI_SOURCE_FX_BROADCAST)
        return;
    if (i.tool && (source != MOVE_MIDI_SOURCE_EXTERNAL || !i.usb.load()))
        return;
    if (!i.tool && count == 3 && (data[0] & 0xf0) == 0x90 && data[2])
        i.incomingChannel = data[0] & 15;
    if (i.tool && i.playing && i.renderFirst)
        return;
    auto &stream = source == MOVE_MIDI_SOURCE_EXTERNAL ? i.external : i.internal;
    if (!stream.input(data, unsigned(count))) {
        ++i.drops;
        i.panic = true;
    }
}
void render(void *object, int16_t *out, int frames) {
    auto &i = *static_cast<Instance *>(object);
    if (i.tool && i.follow && hostApi) {
        if (hostApi->get_clock_status)
            i.clockStatus = hostApi->get_clock_status();
        if (hostApi->get_beat_position)
            i.beat = hostApi->get_beat_position();
        if (hostApi->get_bpm)
            i.tempo = hostApi->get_bpm();
    }
    if (frames <= 0)
        return;
    auto read = i.audioRead.load(std::memory_order_relaxed);
    if (frames != hostFrames || read == i.audioWrite.load(std::memory_order_acquire)) {
        std::fill_n(out, size_t(frames) * 2, 0);
        if (i.active.load())
            ++i.gaps;
        return;
    }
    std::memcpy(out, i.audio[read % queueBlocks].data(), sizeof(Audio));
    i.audioRead.store(read + 1, std::memory_order_release);
}
} // namespace
extern "C" __attribute__((visibility("default"))) plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    hostApi = host;
    static plugin_api_v2_t api{2,
                               create,
                               [](void *p) { delete static_cast<Instance *>(p); },
                               midi,
                               set,
                               get,
                               [](void *, char *out, int n) {
                                   if (n > 0)
                                       *out = 0;
                                   return 0;
                               },
                               render};
    return &api;
}
