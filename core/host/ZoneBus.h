#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <thread>
#include <juce_core/juce_core.h>

// The Zone Bus: instances of the suite in one host process share control-rate state. Never audio.
//
// Change from the plan (docs/DESIGN.md): the handoff kept the registry in a juce::SharedResourcePointer. That object
// is per DLL, and every LAB X-16 effect is its own DLL, so EMISSION and ARTIFACT (or any two different effects) would
// never see each other. The registry is a block of plain data in a named file mapping scoped to the host's process id,
// mapped by every effect DLL; all access is through std::atomic_ref, lock-free on the audio thread.
namespace lab::host::bus
{
    inline constexpr uint32_t magic = 0x3631584Cu;   // "LX16"
    inline constexpr uint32_t layoutVersion = 1;
    inline constexpr int maxPeers = 64;
    inline constexpr int64_t staleMs = 2000;           // a peer silent this long is ignored (suspended or bridged)

    // One slot per living instance. Plain data: every field is read and written with std::atomic_ref.
    struct PeerSlot
    {
        uint32_t id;              // 0 = free; claimed by compare-exchange
        uint32_t code;            // effect code, four characters packed (e.g. "L6Co")
        int64_t heartbeatMs;      // juce::Time::currentTimeMillis() of the last processed block
        int32_t role, state;      // the effect's own small state
        float load;               // CONTAGION viral load 0..1 (published in even bars); 0 for everyone else
        uint32_t reserved;        // CONTAGION: the odd bars' load, as float bits (publishPair)
        uint64_t callsign;        // up to eight characters, packed
    };

    struct EmissionSchedule
    {
        int64_t fireBar = -1;
        int32_t leadBars = 0;
        float severity = 0.0f, pressureSeconds = 0.0f, aftermathSeconds = 0.0f;
        int32_t repeatBars = 0;
        uint32_t sirenId = 0;
        uint32_t seq = 0;
    };

    struct SharedBlock
    {
        uint32_t magicValue, version, initialised, reserved;
        PeerSlot peers[maxPeers];
        uint32_t emissionSeq;                 // seqlock counter: odd while the siren writes
        uint32_t emissionWords[sizeof (EmissionSchedule) / 4];
        uint32_t reseedCounter;               // ARTIFACT listens; incremented when an emission fires
        uint32_t tail[15];
    };

    // Implemented in ZoneBusPlatform.cpp: the process-scoped mapping (null when unavailable).
    SharedBlock* mapSharedBlock() noexcept;

    inline uint64_t packCallsign (const juce::String& text) noexcept
    {
        uint64_t v = 0;
        const auto s = text.toUpperCase().retainCharacters ("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-").substring (0, 8);
        for (int i = 0; i < s.length(); ++i)
            v |= (uint64_t) (uint8_t) s[i] << (8 * i);
        return v;
    }

    inline juce::String unpackCallsign (uint64_t v)
    {
        juce::String s;
        for (int i = 0; i < 8; ++i)
        {
            const auto c = (char) ((v >> (8 * i)) & 0xff);
            if (c == 0)
                break;
            s += juce::String::charToString ((juce::juce_wchar) (uint8_t) c);
        }
        return s;
    }

    inline uint32_t packCode (const char* code) noexcept
    {
        uint32_t v = 0;
        for (int i = 0; i < 4 && code[i] != 0; ++i)
            v |= (uint32_t) (uint8_t) code[i] << (8 * i);
        return v;
    }

    inline juce::String defaultCallsign (uint32_t id)
    {
        return "ST-" + juce::String::toHexString ((int) (id & 0xffff)).toUpperCase().paddedLeft ('0', 4);
    }

    // A peer as seen by a client.
    struct PeerView
    {
        uint32_t id = 0, code = 0;
        int32_t role = 0, state = 0;
        float load = 0.0f, loadOdd = 0.0f;
        uint64_t callsign = 0;
    };

    // Owned by an EffectProcessor when its effect is a bus client. connect / disconnect on the message thread;
    // heartbeat, publish and peer reads are lock-free and safe on the audio thread.
    class Client
    {
    public:
        ~Client() { disconnect(); }

        bool connect (uint32_t id, const char* code, const juce::String& callsign)
        {
            if (slot != nullptr)
            {
                setIdentity (id, callsign);
                return true;
            }
            block = mapSharedBlock();
            if (block == nullptr)
                return false;
            if (! ready())
            {
                block = nullptr;
                return false;
            }
            for (auto& p : block->peers)
            {
                uint32_t expected = 0;
                if (std::atomic_ref<uint32_t> (p.id).compare_exchange_strong (expected, id))
                {
                    slot = &p;
                    myId = id;
                    std::atomic_ref<uint32_t> (p.code).store (packCode (code));
                    std::atomic_ref<int32_t> (p.role).store (0);
                    std::atomic_ref<int32_t> (p.state).store (0);
                    std::atomic_ref<float> (p.load).store (0.0f);
                    std::atomic_ref<uint32_t> (p.reserved).store (0u);
                    std::atomic_ref<uint64_t> (p.callsign).store (packCallsign (callsign));
                    heartbeat();
                    return true;
                }
            }
            block = nullptr;   // all 64 slots taken: this instance runs alone
            return false;
        }

        void disconnect() noexcept
        {
            if (slot != nullptr)
            {
                std::atomic_ref<int64_t> (slot->heartbeatMs).store (0);
                std::atomic_ref<uint32_t> (slot->id).store (0);
            }
            slot = nullptr;
            block = nullptr;
        }

        bool isConnected() const noexcept { return slot != nullptr; }

        void setIdentity (uint32_t id, const juce::String& callsign) noexcept
        {
            if (slot == nullptr)
                return;
            myId = id;
            std::atomic_ref<uint32_t> (slot->id).store (id);
            std::atomic_ref<uint64_t> (slot->callsign).store (packCallsign (callsign));
        }

        void heartbeat() noexcept
        {
            if (slot != nullptr)
                std::atomic_ref<int64_t> (slot->heartbeatMs).store (juce::Time::currentTimeMillis());
        }

        void publish (int32_t role, int32_t state, float load) noexcept
        {
            if (slot == nullptr)
                return;
            std::atomic_ref<int32_t> (slot->role).store (role);
            std::atomic_ref<int32_t> (slot->state).store (state);
            std::atomic_ref<float> (slot->load).store (load);
        }

        // CONTAGION: one load per bar parity, written before the state (release) that names the bar, so a reader that
        // sees the bar sees its load. A reader at bar b reads bar b - 1's load whether or not the writer has already
        // crossed into bar b: the decision never depends on which instance the host processes first.
        void publishParity (int32_t role, int32_t state, int64_t bar, float load) noexcept
        {
            if (slot == nullptr)
                return;
            if ((bar & 1) == 0)
                std::atomic_ref<float> (slot->load).store (load, std::memory_order_relaxed);
            else
            {
                uint32_t bits;
                std::memcpy (&bits, &load, sizeof (bits));
                std::atomic_ref<uint32_t> (slot->reserved).store (bits, std::memory_order_relaxed);
            }
            std::atomic_ref<int32_t> (slot->role).store (role, std::memory_order_relaxed);
            std::atomic_ref<int32_t> (slot->state).store (state, std::memory_order_release);
        }

        // Every other living, fresh peer. Returns the count; fills `out` up to `capacity`.
        int peers (PeerView* out, int capacity) const noexcept
        {
            if (block == nullptr || slot == nullptr)
                return 0;
            const auto now = juce::Time::currentTimeMillis();
            int count = 0;
            for (auto& p : block->peers)
            {
                if (&p == slot)
                    continue;
                const auto id = std::atomic_ref<uint32_t> (p.id).load();
                if (id == 0)
                    continue;
                const auto beat = std::atomic_ref<int64_t> (p.heartbeatMs).load();
                if (beat <= 0 || now - beat > staleMs)
                    continue;
                if (out != nullptr && count < capacity)
                {
                    auto& v = out[count];
                    v.id = id;
                    v.code = std::atomic_ref<uint32_t> (p.code).load();
                    v.state = std::atomic_ref<int32_t> (p.state).load (std::memory_order_acquire);
                    v.role = std::atomic_ref<int32_t> (p.role).load();
                    v.load = std::atomic_ref<float> (p.load).load();
                    const auto bits = std::atomic_ref<uint32_t> (p.reserved).load();
                    std::memcpy (&v.loadOdd, &bits, sizeof (bits));
                    v.callsign = std::atomic_ref<uint64_t> (p.callsign).load();
                }
                ++count;
            }
            return count;
        }

        // EMISSION's schedule: a seqlock. One writer at a time (the siren that owns the schedule).
        void postSchedule (const EmissionSchedule& s) noexcept
        {
            if (block == nullptr)
                return;
            std::atomic_ref<uint32_t> seq (block->emissionSeq);
            const auto start = seq.load (std::memory_order_relaxed);
            seq.store (start + 1, std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_release);
            uint32_t words[sizeof (EmissionSchedule) / 4];
            std::memcpy (words, &s, sizeof (EmissionSchedule));
            for (size_t i = 0; i < std::size (words); ++i)
                std::atomic_ref<uint32_t> (block->emissionWords[i]).store (words[i], std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_release);
            seq.store (start + 2, std::memory_order_release);
        }

        bool readSchedule (EmissionSchedule& out) const noexcept
        {
            if (block == nullptr)
                return false;
            std::atomic_ref<uint32_t> seq (block->emissionSeq);
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                const auto before = seq.load (std::memory_order_acquire);
                if ((before & 1u) != 0)
                    continue;
                uint32_t words[sizeof (EmissionSchedule) / 4];
                for (size_t i = 0; i < std::size (words); ++i)
                    words[i] = std::atomic_ref<uint32_t> (block->emissionWords[i]).load (std::memory_order_relaxed);
                std::atomic_thread_fence (std::memory_order_acquire);
                if (seq.load (std::memory_order_relaxed) == before)
                {
                    std::memcpy (&out, words, sizeof (EmissionSchedule));
                    return true;
                }
            }
            return false;
        }

        uint32_t reseedCount() const noexcept
        {
            return block != nullptr ? std::atomic_ref<uint32_t> (block->reseedCounter).load() : 0u;
        }

        void bumpReseed() noexcept
        {
            if (block != nullptr)
                std::atomic_ref<uint32_t> (block->reseedCounter).fetch_add (1);
        }

    private:
        // The first mapper of a fresh (zeroed) block stamps it; others wait for the stamp and check the layout.
        bool ready() noexcept
        {
            std::atomic_ref<uint32_t> init (block->initialised);
            uint32_t expected = 0;
            if (init.compare_exchange_strong (expected, 1))
            {
                block->magicValue = magic;
                block->version = layoutVersion;
                init.store (2, std::memory_order_release);
            }
            for (int spin = 0; spin < 100000 && init.load (std::memory_order_acquire) != 2; ++spin)
                std::this_thread::yield();
            return init.load (std::memory_order_acquire) == 2 && block->magicValue == magic && block->version == layoutVersion;
        }

        SharedBlock* block = nullptr;
        PeerSlot* slot = nullptr;
        uint32_t myId = 0;
    };
}
