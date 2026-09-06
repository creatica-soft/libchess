// Self-describing UCI options.
//
// This replaces two things that did not work. The engine's tunables used to live in fixed
// enums (EngineSpinOptions and friends) inside libchess.h, so adding one knob meant editing
// the library header and rebuilding a 110 MB shared object -- which is precisely why the
// policy-head settings ended up as getenv() calls instead: the right mechanism was too
// expensive to use. It also meant the same option arrays were indexed two incompatible
// ways, by a fixed enum for creatica's own options and by discovery order in engine.cpp
// when parsing what an EXTERNAL engine advertises.
//
// Here an engine declares what it has, once, and everything else follows: the "option name
// ..." block sent in response to "uci" is generated from the declarations, "setoption" is
// dispatched by name, and bounds are enforced in one place. An option with no name cannot
// be advertised, which is how the old table came to emit
//     option name  type spin default 0 min 0 max 0
//
// Nothing here belongs in libchess. These are one engine's settings, not chess.
#ifndef UCI_OPTIONS_H
#define UCI_OPTIONS_H

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

namespace uci {

// Case-insensitive compare: the UCI spec says option names are case sensitive, but GUIs
// are inconsistent enough in practice that being strict only produces silent no-ops.
inline bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

class Options {
public:
    enum Type { Check, Spin, String, Button };

    struct Entry {
        std::string name;
        Type type = Spin;
        // exactly one of these is non-null, except for Button
        bool*        b_slot = nullptr;
        int64_t*     i_slot = nullptr;
        double*      d_slot = nullptr;
        std::string* s_slot = nullptr;
        // spin bounds, in UCI units (already scaled)
        int64_t lo = 0, hi = 0, def_i = 0;
        bool    def_b = false;
        std::string def_s;
        // A real-valued option is carried as a scaled integer, because UCI has no float
        // type. scale=100 means the GUI shows 45 and the engine sees 0.45. Keeping the
        // scaling here means engine code never deals in hundredths.
        int scale = 1;
        std::function<void()> on_change;
        std::function<void()> action;   // Button only
    };

    void check(const char* name, bool* slot, bool def,
               std::function<void()> on_change = {}) {
        Entry e; e.name = name; e.type = Check; e.b_slot = slot; e.def_b = def;
        e.on_change = std::move(on_change);
        *slot = def;
        entries_.push_back(std::move(e));
    }

    void spin(const char* name, int64_t* slot, int64_t def, int64_t lo, int64_t hi,
              std::function<void()> on_change = {}) {
        Entry e; e.name = name; e.type = Spin; e.i_slot = slot;
        e.def_i = def; e.lo = lo; e.hi = hi;
        e.on_change = std::move(on_change);
        *slot = def;
        entries_.push_back(std::move(e));
    }

    // Real-valued knob presented to the GUI as an integer spin.
    //   real("PolicyBlend", &blend, 0.45, 0.0, 1.0, 100)
    // advertises  min 0 max 100 default 45  and stores 0.45.
    void real(const char* name, double* slot, double def, double lo, double hi,
              int scale = 100, std::function<void()> on_change = {}) {
        Entry e; e.name = name; e.type = Spin; e.d_slot = slot; e.scale = scale;
        e.def_i = (int64_t)llround(def * scale);
        e.lo    = (int64_t)llround(lo  * scale);
        e.hi    = (int64_t)llround(hi  * scale);
        e.on_change = std::move(on_change);
        *slot = def;
        entries_.push_back(std::move(e));
    }

    void string(const char* name, std::string* slot, const char* def,
                std::function<void()> on_change = {}) {
        Entry e; e.name = name; e.type = String; e.s_slot = slot; e.def_s = def ? def : "";
        e.on_change = std::move(on_change);
        *slot = e.def_s;
        entries_.push_back(std::move(e));
    }

    void button(const char* name, std::function<void()> action) {
        Entry e; e.name = name; e.type = Button; e.action = std::move(action);
        entries_.push_back(std::move(e));
    }

    // The whole "option name ..." block, in declaration order.
    void print(FILE* out) const {
        for (const Entry& e : entries_) {
            if (e.name.empty()) continue;          // cannot happen by construction; cheap guard
            switch (e.type) {
            case Check:
                std::fprintf(out, "option name %s type check default %s\n",
                             e.name.c_str(), e.def_b ? "true" : "false");
                break;
            case Spin:
                std::fprintf(out, "option name %s type spin default %lld min %lld max %lld\n",
                             e.name.c_str(), (long long)e.def_i,
                             (long long)e.lo, (long long)e.hi);
                break;
            case String:
                std::fprintf(out, "option name %s type string default %s\n",
                             e.name.c_str(),
                             e.def_s.empty() ? "<empty>" : e.def_s.c_str());
                break;
            case Button:
                std::fprintf(out, "option name %s type button\n", e.name.c_str());
                break;
            }
        }
    }

    // Apply "setoption name <name> value <value>". Returns false if the name is unknown,
    // so the caller can say so rather than failing silently.
    bool set(const std::string& name, const std::string& value) {
        for (Entry& e : entries_) {
            if (!iequals(e.name, name)) continue;
            switch (e.type) {
            case Check:
                *e.b_slot = (value == "true" || value == "1");
                break;
            case Spin: {
                int64_t v = std::strtoll(value.c_str(), nullptr, 10);
                if (v < e.lo) v = e.lo;
                if (v > e.hi) v = e.hi;              // clamped, never rejected
                if (e.d_slot) *e.d_slot = (double)v / e.scale;
                else if (e.i_slot) *e.i_slot = v;
                break;
            }
            case String:
                *e.s_slot = (value == "<empty>") ? "" : value;
                break;
            case Button:
                if (e.action) e.action();
                break;
            }
            if (e.on_change) e.on_change();
            return true;
        }
        return false;
    }

    // For diagnostics: one line summarising every current value. Worth logging at startup,
    // because "which settings did that match actually run with" is otherwise unanswerable
    // after the fact.
    std::string summary() const {
        std::string s;
        char buf[128];
        for (const Entry& e : entries_) {
            if (e.type == Button) continue;
            if (!s.empty()) s += ", ";
            s += e.name + " ";
            if (e.type == Check)       s += (*e.b_slot ? "true" : "false");
            else if (e.type == String) s += (e.s_slot->empty() ? "<empty>" : *e.s_slot);
            else if (e.d_slot)       { std::snprintf(buf, sizeof buf, "%.4g", *e.d_slot); s += buf; }
            else                     { std::snprintf(buf, sizeof buf, "%lld", (long long)*e.i_slot); s += buf; }
        }
        return s;
    }

    const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
};

} // namespace uci

#endif
