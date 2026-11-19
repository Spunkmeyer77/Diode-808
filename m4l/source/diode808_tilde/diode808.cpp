#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX // the Max SDK pulls in windows.h: keep its min/max macros off std::min/std::max
#endif
// diode808~ — the Diode 808 circuit engine for Max for Live (DIODE 808 device). Transistor Labs.
// 15 signal inlets: one trigger per sequencer track (BD SD LT MT HT RS CP CB CY CH OH LC MC HC CL).
// A trigger is a non-zero sample after a zero one; its value: 0.5 = normal, 1.0 = accented, +4 = flam.
// Engine: tr808::VoiceBoard (Lemus transistor-level 808), GNU AGPLv3.
#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include "tr808/core/voiceboard.h"
#include "tr808/core/parallel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace {
using namespace tr808;

struct Control {
    const char* name;
    double Controls::* knob;
    bool Controls::* selector;
};
constexpr Control controls[] = {
#include "controls.inc"
};
constexpr auto controlCount = sizeof(controls) / sizeof(*controls);

enum class CalibrationIndex {
#define CAL(key, label, unit, low, high, initial) key,
#include "calibration.inc"
#undef CAL
    count
};

struct CalibrationControl {
    const char* name;
    double minimum, maximum, initial;
};

constexpr CalibrationControl calibrationControls[] = {
#define CAL(key, label, unit, low, high, initial) {#key, low, high, initial},
#include "calibration.inc"
#undef CAL
};

using CalibrationValues = std::array<double, static_cast<size_t>(CalibrationIndex::count)>;

Calibration makeCalibration(const CalibrationValues& values) {
    auto value = [&](CalibrationIndex index) { return values[static_cast<size_t>(index)]; };
    auto cal = unitCalibration();
    using C = CalibrationIndex;
    cal.cbHz1 = value(C::cbHz1);
    cal.cbHz2 = value(C::cbHz2);
    cal.metalHz = {value(C::metalHz1), value(C::metalHz2), value(C::metalHz3), value(C::metalHz4)};
    cal.metalDuty = value(C::metalDuty) / 100;
    cal.noiseRms = value(C::noiseRms) / 1000;
    cal.cpOffset = value(C::cpOffset) / 100;
    cal.lateClap = value(C::lateClap) >= 0.5;
    cal.lateSnare = value(C::lateSnare) >= 0.5;
    cal.claveR313 = value(C::claveR313) * 1000;
    cal.parts.erase("rs.r313");
    cal.monoMs = value(C::monoMs);
    const double cap = 15e-9 * std::exp2(-value(C::kickTune) / 12);
    cal.parts["bd.c41"] = cap;
    cal.parts["bd.c42"] = cap;
    return cal;
}

// Denormals: a decaying circuit's tiny values slow the CPU down. Flush them to zero on the audio
// thread and on the worker threads (the bit is per thread).
struct NoDenormals {
#if defined(__aarch64__)
    uint64_t saved;
    NoDenormals() {
        asm volatile("mrs %0, fpcr" : "=r"(saved));
        asm volatile("msr fpcr, %0" : : "r"(saved | (uint64_t(1) << 24)));
    }
    ~NoDenormals() { asm volatile("msr fpcr, %0" : : "r"(saved)); }
#elif defined(__x86_64__) || defined(_M_X64)
    unsigned saved;
    NoDenormals() : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040); }
    ~NoDenormals() { _mm_setcsr(saved); }
#endif
};

void flushDenormalsForever() {
#if defined(__aarch64__)
    uint64_t r;
    asm volatile("mrs %0, fpcr" : "=r"(r));
    asm volatile("msr fpcr, %0" : : "r"(r | (uint64_t(1) << 24)));
#elif defined(__x86_64__) || defined(_M_X64)
    _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
}

// The voice workers render inside the audio callback, so they need audio-thread priority: on macOS
// a real-time (time-constraint) thread, which also keeps them on the M1's performance cores.
void makeWorkerRealtime(int) {
    flushDenormalsForever();
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#elif defined(__APPLE__)
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const double msToAbs = 1e6 * double(tb.denom) / double(tb.numer);
    thread_time_constraint_policy_data_t policy;
    policy.period = uint32_t(2.9 * msToAbs);       // about 128 samples at 44.1 kHz
    policy.computation = uint32_t(1.0 * msToAbs);
    policy.constraint = uint32_t(2.9 * msToAbs);
    policy.preemptible = 1;
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#elif defined(__linux__)
    sched_param sp{};
    sp.sched_priority = 70;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); // ignored if not permitted
#endif
}

int workerCount() {
    // The calling audio thread takes jobs too; two helpers suit an M1's four performance cores and
    // leave room for other devices and instances.
    const unsigned cores = std::thread::hardware_concurrency();
#if defined(__linux__)
    return cores >= 4 ? 1 : 0;
#else
    return cores >= 4 ? 2 : (cores >= 2 ? 1 : 0);
#endif
}


constexpr int kInputs = 15;
// inlet -> board voice bit, and the shared-circuit group (-1, or 0..3 = LT/LC, MT/MC, HT/HC, RS/CL)
constexpr uint16_t kInputBit[kInputs] = {kBitBD, kBitSD, kBitLT, kBitMT, kBitHT, kBitRS, kBitCP, kBitCB,
                                         kBitCY, kBitCH, kBitOH, kBitLT, kBitMT, kBitHT, kBitRS};
constexpr int kInputGroup[kInputs] = {-1, -1, 0, 1, 2, 3, -1, -1, -1, -1, -1, 0, 1, 2, 3};
constexpr bool kInputAlt[kInputs] = {false, false, false, false, false, false, false, false,
                                     false, false, false, true, true, true, true};

struct Hit { long at; int input; bool accent; };
struct Flam { long left; int input; bool accent; };

struct Engine {
    // Declared before the board: the board holds a pointer to it, so it must outlive the board.
    std::unique_ptr<Parallel> parallel = std::make_unique<Parallel>(workerCount(), makeWorkerRealtime);
    std::unique_ptr<VoiceBoard> board = std::make_unique<VoiceBoard>();
    std::mutex boardMutex;
    std::array<std::atomic<double>, static_cast<size_t>(CalibrationIndex::count)> calibration;
    CalibrationValues built{};
    CalibrationValues calApplied{};
    double kickNow = 0, kickTarget = 0;
    double sampleRate = 0;
    bool syncControls = true;
    std::array<std::atomic<float>, controlCount> values;
    std::atomic<float> flam{0.4f};
    std::atomic<bool> eco{false};                 // ECO: no 2x oversampling               // FLAM knob 0..1 = 5..40 ms
    Controls applied{};
    bool sel[4] = {false, false, false, false};  // conga / claves selected on the shared circuits
    uint16_t pending = 0;                        // hits waiting for the next common trigger
    int hold = 0;
    int pulseSamples = 0;
    double last[kInputs] = {};
    std::vector<float> buffer;
    std::vector<Hit> hits;
    std::vector<Flam> flams;
    std::atomic<bool> prepared{false};

    Engine() {
        for (size_t i = 0; i < built.size(); ++i) {
            built[i] = calibrationControls[i].initial;
            calibration[i].store(built[i], std::memory_order_relaxed);
        }
        calApplied = built;
        board->setCalibration(makeCalibration(built));
        board->setParallel(parallel->workers() > 0 ? parallel.get() : nullptr);
        for (size_t i = 0; i < controlCount; ++i) values[i].store(applied.*controls[i].knob);
        hits.reserve(256);
        flams.reserve(64);
    }
};

struct External {
    t_pxobject object;
    Engine* engine;
    void* statusOutlet;
};
t_class* engineClass = nullptr;

void control(External* x, t_symbol*, long count, t_atom* atoms) {
    if (count != 2 || atom_gettype(atoms) != A_SYM) return;
    const auto* name = atom_getsym(atoms);
    const double value = atom_getfloat(atoms + 1);
    if (!std::isfinite(value)) return;
    for (size_t i = 0; i < controlCount; ++i) {
        if (name == gensym(controls[i].name)) {
            x->engine->values[i].store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
            return;
        }
    }
}

void setFlam(External* x, double value) {
    if (std::isfinite(value)) x->engine->flam.store(static_cast<float>(std::clamp(value, 0.0, 1.0)), std::memory_order_relaxed);
}
double kickCap(double semitones) { return 15e-9 * std::exp2(-semitones / 12); }

constexpr int kGlideSamples = 16;     // a C41/C42 update every 16 samples while gliding
constexpr double kGlideSeconds = 0.02; // the glide's time constant

// One glide step: C41 and C42 a little closer to the knob. True while still gliding.
bool glideKick(Engine& engine, int samples) {
    const double gap = engine.kickTarget - engine.kickNow;
    if (std::abs(gap) < 1e-4) {
        if (gap != 0) {
            engine.kickNow = engine.kickTarget;
            const double cap = kickCap(engine.kickNow);
            engine.board->setLivePart(Voice::BD, "c41", cap);
            engine.board->setLivePart(Voice::BD, "c42", cap);
        }
        return false;
    }
    engine.kickNow += gap * (1 - std::exp(-samples / (kGlideSeconds * engine.sampleRate)));
    const double cap = kickCap(engine.kickNow);
    engine.board->setLivePart(Voice::BD, "c41", cap);
    engine.board->setLivePart(Voice::BD, "c42", cap);
    return true;
}

// Audio thread: apply the calibration values that moved since the last block.
void applyLiveCalibration(Engine& engine) {
    using C = CalibrationIndex;
    CalibrationValues now;
    bool any = false;
    for (size_t i = 0; i < now.size(); ++i) {
        now[i] = engine.calibration[i].load(std::memory_order_relaxed);
        any |= now[i] != engine.calApplied[i];
    }
    if (!any) return;
    auto moved = [&](C c) { return now[static_cast<size_t>(c)] != engine.calApplied[static_cast<size_t>(c)]; };
    auto value = [&](C c) { return now[static_cast<size_t>(c)]; };
    auto& board = *engine.board;
    if (moved(C::cbHz1) || moved(C::cbHz2) || moved(C::metalHz1) || moved(C::metalHz2) ||
        moved(C::metalHz3) || moved(C::metalHz4) || moved(C::metalDuty)) {
        const double hz[6] = {value(C::cbHz1), value(C::cbHz2), value(C::metalHz1),
                              value(C::metalHz2), value(C::metalHz3), value(C::metalHz4)};
        board.setLiveMetal(hz, value(C::metalDuty) / 100);
    }
    if (moved(C::noiseRms)) board.setLiveNoise(value(C::noiseRms) / 1000);
    if (moved(C::cpOffset)) board.setLiveClapOffset(value(C::cpOffset) / 100);
    if (moved(C::lateClap)) // R200 (10k, Q70's emitter to -15 V) fitted in later units, open in early ones
        board.setLivePart(Voice::CP, "r200cp", value(C::lateClap) >= 0.5 ? 10e3 : 1e12);
    if (moved(C::lateSnare)) { // the "PORTION CHANGED" values, or the schematic's (sd.cir)
        const bool late = value(C::lateSnare) >= 0.5;
        board.setLivePart(Voice::SD, "c58", late ? 0.056e-6 : 0.027e-6);
        board.setLivePart(Voice::SD, "c61", late ? 0.015e-6 : 0.0068e-6);
        board.setLivePart(Voice::SD, "r191", late ? 10e3 : 8.2e3);
        board.setLivePart(Voice::SD, "r200", late ? 0.1 : 47e3);
    }
    if (moved(C::claveR313)) board.setLivePart(Voice::RS, "r313", value(C::claveR313) * 1000);
    if (moved(C::monoMs)) {
        board.setLiveTriggerWidth(value(C::monoMs));
        engine.pulseSamples = std::max(1, static_cast<int>(std::ceil(engine.sampleRate * (value(C::monoMs) + 0.2) * 0.001)));
    }
    if (moved(C::kickTune)) engine.kickTarget = value(C::kickTune); // perform() glides C41/C42 there
    engine.calApplied = now;
}


void applyControls(Engine& engine, bool force) {
    auto next = engine.applied;
    bool changed = force;
    for (size_t i = 0; i < controlCount; ++i) {
        const float value = engine.values[i].load(std::memory_order_relaxed);
        changed |= next.*controls[i].knob != value;
        next.*controls[i].knob = value;
    }
    bool* sw[4] = {&next.lc, &next.mc, &next.hc, &next.cl};
    for (int g = 0; g < 4; ++g) { changed |= *sw[g] != engine.sel[g]; *sw[g] = engine.sel[g]; }
    next.ma = false;
    if (changed) {
        engine.board->setControls(next);
        engine.applied = next;
    }
}

// The hits that land on one sample: the shared circuits take the sound that was asked for;
// when a pair's two sounds land together, the alternate one (conga / claves) plays.
void takeHits(Engine& engine, size_t& h, long at) {
    int want[4] = {-1, -1, -1, -1};
    for (; h < engine.hits.size() && engine.hits[h].at == at; ++h) {
        const auto& hit = engine.hits[h];
        const int g = kInputGroup[hit.input];
        if (g >= 0) want[g] = std::max(want[g], kInputAlt[hit.input] ? 1 : 0);
        engine.pending |= kInputBit[hit.input];
        if (hit.accent) engine.pending |= kBitAC;
    }
    bool moved = false;
    for (int g = 0; g < 4; ++g)
        if (want[g] >= 0 && engine.sel[g] != (want[g] == 1)) { engine.sel[g] = want[g] == 1; moved = true; }
    if (moved) applyControls(engine, false);
}

void perform(External* x, t_object*, double** ins, long, double** outs, long,
             long frames, long, void*) {
    auto& engine = *x->engine;
    const NoDenormals noDenormals;
    std::unique_lock lock(engine.boardMutex, std::try_to_lock);
    if (!lock.owns_lock() || !engine.prepared.load(std::memory_order_relaxed)) {
        std::fill_n(outs[0], frames, 0.0);
        return;
    }
    applyControls(engine, engine.syncControls);
    engine.syncControls = false;
    applyLiveCalibration(engine);

    // 1. this block's hits: flam main hits that fall due, then the trigger inlets
    auto& hits = engine.hits;
    hits.clear();
    for (size_t f = 0; f < engine.flams.size();) {
        auto& fl = engine.flams[f];
        if (fl.left < frames) { hits.push_back({fl.left, fl.input, fl.accent}); engine.flams.erase(engine.flams.begin() + long(f)); }
        else { fl.left -= frames; ++f; }
    }
    const long flamSamples = std::max<long>(1, std::lround((5.0 + 35.0 * engine.flam.load(std::memory_order_relaxed)) * 0.001 * engine.sampleRate));
    for (int v = 0; v < kInputs; ++v) {
        const double* in = ins[v];
        double prev = engine.last[v];
        for (long i = 0; i < frames; ++i) {
            const double s = in[i];
            if (s != 0 && prev == 0) {
                const bool flam = s >= 3.0;
                const bool accent = (flam ? s - 4.0 : s) >= 0.75;
                if (flam) {                       // grace note now, unaccented; the main hit follows
                    hits.push_back({i, v, false});
                    const long at = i + flamSamples;
                    if (at < frames) hits.push_back({at, v, accent});
                    else if (engine.flams.size() < 64) engine.flams.push_back({at - frames, v, accent});
                } else {
                    hits.push_back({i, v, accent});
                }
            }
            prev = s;
        }
        engine.last[v] = prev;
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.at < b.at; });

    // 2. render: the common trigger rule of the 808 (one step pulse, data held pulseSamples)
    size_t h = 0;
    long position = 0;
    float* out = engine.buffer.data();
    while (position < frames) {
        if (h < hits.size() && hits[h].at <= position) { takeHits(engine, h, hits[h].at); continue; }
        if (!engine.hold && engine.pending) {
            const auto bits = engine.pending;
            engine.pending = 0;
            engine.board->setLines(true, bits);
            engine.board->render(out + position, 1);
            ++position;
            engine.board->setLines(false, bits);
            engine.hold = engine.pulseSamples;
            continue;
        }
        if (!engine.hold) engine.board->setLines(false, 0);
        long block = frames - position;
        if (h < hits.size()) block = std::min(block, hits[h].at - position);
        if (engine.hold) block = std::min<long>(block, engine.hold);
        if (engine.kickNow != engine.kickTarget) {
            block = std::min<long>(block, kGlideSamples);
            glideKick(engine, static_cast<int>(block));
        }
        block = std::max<long>(1, block);
        engine.board->render(out + position, static_cast<int>(block));
        position += block;
        if (engine.hold) engine.hold = std::max(0, engine.hold - static_cast<int>(block));
    }
    for (long i = 0; i < frames; ++i) outs[0][i] = out[i];
}

void calibration(External* x, t_symbol*, long count, t_atom* atoms) {
    if (count != 2 || atom_gettype(atoms) != A_SYM) return;
    const auto* name = atom_getsym(atoms);
    const double value = atom_getfloat(atoms + 1);
    if (!std::isfinite(value)) return;
    auto& engine = *x->engine;
    for (size_t i = 0; i < engine.calibration.size(); ++i) {
        const auto& item = calibrationControls[i];
        if (name == gensym(item.name)) {
            engine.calibration[i].store(std::clamp(value, item.minimum, item.maximum), std::memory_order_relaxed);
            return;
        }
    }
}

void status(External* x) {
    outlet_anything(x->statusOutlet,
        gensym(x->engine->prepared.load(std::memory_order_relaxed) ? "armed" : "loaded"), 0, nullptr);
}

// (Re)builds the board at this rate; the caller holds boardMutex. Live calibration is applied
// again from the built values by applyLiveCalibration.
void prepareBoard(Engine& engine, double rate) {
    engine.board->setEco(engine.eco.load(std::memory_order_relaxed));
    engine.board->prepare(rate);
    engine.sampleRate = rate;
    engine.calApplied = engine.built;
    engine.kickNow = engine.built[static_cast<size_t>(CalibrationIndex::kickTune)];
    const auto width = engine.board->calibration().monoMs;
    engine.pulseSamples = std::max(1, static_cast<int>(std::ceil(rate * (width + 0.2) * 0.001)));
    engine.syncControls = true;
    engine.hold = 0;
    engine.pending = 0;
    engine.flams.clear();
    for (auto& l : engine.last) l = 0;
}

// ECO on/off: the board without its 2x oversampling. Rebuilt here (a short gap in the sound).
void setEco(External* x, double value) {
    const bool on = value >= 0.5;
    auto& engine = *x->engine;
    if (engine.eco.exchange(on) == on) return;
    if (engine.sampleRate <= 0) return;
    try {
        const std::lock_guard lock(engine.boardMutex);
        engine.prepared.store(false, std::memory_order_relaxed);
        prepareBoard(engine, engine.sampleRate);
        engine.prepared.store(true, std::memory_order_relaxed);
    } catch (const std::exception& error) {
        object_error(reinterpret_cast<t_object*>(x), "Could not switch ECO: %s", error.what());
    }
}

void dsp64(External* x, t_object* chain, short*, double rate, long maxFrames, long) {
    auto& engine = *x->engine;
    engine.prepared.store(false, std::memory_order_relaxed);
    if (rate <= 0 || maxFrames <= 0) return;
    try {
        const std::lock_guard lock(engine.boardMutex);
        engine.buffer.resize(maxFrames);
        prepareBoard(engine, rate);
        dsp_add64(chain, reinterpret_cast<t_object*>(x), reinterpret_cast<t_perfroutine64>(perform), 0, nullptr);
        engine.prepared.store(true, std::memory_order_relaxed);
    } catch (const std::exception& error) {
        object_error(reinterpret_cast<t_object*>(x), "Could not prepare voice board: %s", error.what());
    }
}

void assist(External*, void*, long io, long index, char* text) {
    static const char* names[kInputs] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP", "CB", "CY", "CH", "OH", "LC", "MC", "HC", "CL"};
    if (io == ASSIST_INLET) snprintf(text, 256, "%s trigger (signal: 0.5 normal, 1 accent, +4 flam)%s",
                                     index >= 0 && index < kInputs ? names[index] : "?", index == 0 ? "; control / cal / flam messages" : "");
    else snprintf(text, 256, index == 0 ? "Mix (signal)" : "Status");
}

void destroy(External* x) {
    dsp_free(&x->object);
    delete x->engine;
}

void* create() {
    auto* x = reinterpret_cast<External*>(object_alloc(engineClass));
    if (!x) return nullptr;
    x->engine = nullptr;
    dsp_setup(&x->object, kInputs);
    x->object.z_misc |= Z_NO_INPLACE;
    try {
        x->engine = new Engine();
    } catch (const std::exception& error) {
        object_error(reinterpret_cast<t_object*>(x), "Could not create voice board: %s", error.what());
        object_free(x);
        return nullptr;
    }
    x->statusOutlet = outlet_new(reinterpret_cast<t_object*>(x), nullptr);
    outlet_new(reinterpret_cast<t_object*>(x), "signal");
    return x;
}
}

extern "C" C74_EXPORT void ext_main(void*) {
    auto* cls = class_new("diode808~", reinterpret_cast<method>(create),
                          reinterpret_cast<method>(destroy), sizeof(External), nullptr, 0);
    class_addmethod(cls, reinterpret_cast<method>(dsp64), "dsp64", A_CANT, 0);
    class_addmethod(cls, reinterpret_cast<method>(control), "control", A_GIMME, 0);
    class_addmethod(cls, reinterpret_cast<method>(calibration), "cal", A_GIMME, 0);
    class_addmethod(cls, reinterpret_cast<method>(setFlam), "flam", A_FLOAT, 0);
    class_addmethod(cls, reinterpret_cast<method>(setEco), "eco", A_FLOAT, 0);
    class_addmethod(cls, reinterpret_cast<method>(status), "status", 0);
    class_addmethod(cls, reinterpret_cast<method>(assist), "assist", A_CANT, 0);
    class_dspinit(cls);
    class_register(CLASS_BOX, cls);
    engineClass = cls;
    post("diode808~ 1.1.0 (DIODE 808): Diode 808 circuit engine");
}
