// Launcher settings, validation and persistence. Shared by the iOS UI, the Mac app's settings window (dm/mac/Launcher.swift) and the native storage regression test.
import Foundation
import Combine

struct Setting: Decodable, Identifiable {
    let env, kind, label, note, def: String
    let section: String?   // heading on the settings screen; absent in older bundles
    let needs: String?     // "<VAR>=<value>[|<value>...]": a sub-setting, drawn only while that setting has one of the values
    let conflicts: String? // "<VAR>=<value>": held at its default, and drawn disabled, while that setting has that value; "<VAR>=<value>:<own>[|<own>...]": only those values of this row;
                           // several such clauses joined by ","
    let level: String?     // "basic": shown first (the Mac window's Basic settings); any other or none: Advanced settings. The iPhone keeps one list
    let atleast: String?   // "<VAR>": a choice of numbers never below the number that setting holds: lower values are not offered, a lower one held is raised
    let base: Base?        // render scale: the 3D views' size at 1.0, docked and handheld, for the size in pixels (the Mac window)
    let rateBase: Base?    // a choice of upscale rates: the output size each rate divides, docked and handheld, for each option's render size in pixels (the Mac window)
    let rates: String?     // "<value>:<rate>[|<value>:<rate>...]": values of a rate choice that are not numbers, and the rate each renders at (Bananza's native-mfx: 1, its upscaler on)
    // A size per console mode, and others while a setting has a value (when: the first entry whose "<VAR>=<value>" holds wins; Bananza's output with Output resolution: Window).
    // display: the docked size is the screen's, known at launch only (Match display): LauncherModel.displaySizes when the front end gives them, else none
    struct Base: Decodable {
        let docked, handheld: [Int]
        let when: [Alt]?
        struct Alt: Decodable { let `is`: String; let docked, handheld: [Int]; let display: Bool? }
    }
    let options: [[String]]?   // choice: [value, label]
    let min, max, step: Double?   // range
    let on, off: String?   // toggle: the values the variable takes (1 / 0 when not given)
    var id: String { env }
    var sectionTitle: String { section ?? "Game" }
    var basic: Bool { level == "basic" }
    var onValue: String { on ?? "1" }
    var offValue: String { off ?? "0" }
    // The rate a value of a rate choice renders at: the number itself, or the one rates gives a value that is not a number (nil: neither, such as game)
    func rate(_ v: String) -> Double? {
        if let r = Double(v) { return r }
        for p in (rates ?? "").split(separator: "|") { let kv = p.split(separator: ":", maxSplits: 1); if kv.count == 2, kv[0] == v { return Double(kv[1]) } }
        return nil
    }
    // A value rates gives a rate (not a number itself): at rate 1 its upscaler still runs, anti-aliasing at the output's size
    func aliased(_ v: String) -> Bool { Double(v) == nil && rate(v) != nil }
    func isDefault(_ v: String) -> Bool {   // a range compares as numbers (a slider writes 1.00 for 1.0)
        kind == "range" ? abs((Double(v) ?? .nan) - (Double(def) ?? .nan)) < 1e-6 : v == def
    }
    func accepts(_ v: String) -> Bool {   // a saved choice the game still offers
        switch kind {
        case "choice": return options?.contains { $0[0] == v } ?? false
        case "range":
            guard !v.contains(where: { $0 == "x" || $0 == "X" }), let n = Double(v), n.isFinite, let lo = min, let hi = max else { return false }   // no hex: launcher.c's check agrees
            return n >= lo && n <= hi
        case "toggle": return v == onValue || v == offValue
        default: return true
        }
    }
    enum CodingKeys: String, CodingKey { case env, kind, label, note, def = "default", section, needs, conflicts, level, atleast, base, rateBase = "rate_base", rates, options, min, max, step, on, off }
}
struct SettingsFile: Decodable { let settings: [Setting] }

final class LauncherModel: ObservableObject {
    let settings: [Setting]
    let fixed: Set<String>      // set by an env file or by the launch environment: shown, not changed
    let fromLaunch: Set<String>   // of those, the ones the launch environment set (a front end's launch overrides, Xcode; DM_ENV_LAUNCH_KEYS from launcher.c or main.m)
    let ignoreSaved: Bool   // DM_SAVED_CHOICES=0: start from the defaults, and leave the saved choices as they are
    @Published var values: [String: String] = [:]
    // Match display's window size (a when entry with display), fullscreen (true) and as the game window opens (false): the Mac window sets them from the screen at launch
    // (dm/host/match_display.h, as the host picks it); empty elsewhere, which shows no render sizes for it
    var displaySizes: [Bool: [Int]] = [:]

    private let defaults: UserDefaults

    init(_ settings: [Setting], defaults: UserDefaults = .standard) {
        self.defaults = defaults
        self.settings = settings
        let env = ProcessInfo.processInfo.environment
        fromLaunch = Set((env["DM_ENV_LAUNCH_KEYS"] ?? "").split(separator: " ").map(String.init))
        fixed = fromLaunch.union((env["DM_ENV_FILE_KEYS"] ?? "").split(separator: " ").map(String.init))
        ignoreSaved = env["DM_SAVED_CHOICES"] == "0"
        if ignoreSaved { fputs("launcher: saved choices ignored (DM_SAVED_CHOICES=0)\n", stderr) }
        for s in settings {
            if fixed.contains(s.env), let v = env[s.env] { values[s.env] = v; continue }
            let saved = ignoreSaved ? nil : defaults.object(forKey: "launcher." + s.env) as? String   // a string, as launcher.c reads it: not string(forKey:), which turns a number into one
            values[s.env] = saved.flatMap { s.accepts($0) ? $0 : nil } ?? s.def   // a saved choice the game no longer offers: its default
        }
        for s in settings where held(s) && !fixed.contains(s.env) { values[s.env] = s.def }   // two saved choices that conflict: the later row in the list keeps its own
        for s in settings where !fixed.contains(s.env) { raise(s) }   // a saved value below its atleast bound: the lowest one offered
    }

    // A row's "conflicts" as its clauses (joined by ","), each as its parts: the setting, its value, and this row's own values that cannot go with it (nil: the whole row)
    private func conflicts(_ s: Setting) -> [(key: String, want: String, own: [String]?)] {
        (s.conflicts ?? "").split(separator: ",").compactMap { c in
            guard let eq = c.firstIndex(of: "=") else { return nil }
            let rest = c[c.index(after: eq)...], colon = rest.firstIndex(of: ":")
            let want = String(colon.map { rest[rest.startIndex..<$0] } ?? rest)
            let own = colon.map { rest[rest.index(after: $0)...].split(separator: "|").map(String.init) }
            return (String(c[c.startIndex..<eq]), want, own)
        }
    }
    // The clauses whose other row has the value this row conflicts with now (drawn, so its value counts): that row, and this row's values it rules out (nil: all)
    private func conflicting(_ s: Setting) -> [(other: Setting, own: [String]?)] {
        conflicts(s).compactMap { c in
            guard values[c.key] == c.want, let other = settings.first(where: { $0.env == c.key }), visible(other) else { return nil }
            return (other, c.own)
        }
    }
    // A row with "conflicts" cannot be used together with that setting's value: while the other row has it, this one is held at
    // its default and drawn disabled. Choosing a value resets the rows that conflict with it, so the two never hold together; a value an env file fixes stays.
    // A conflict of some of the row's own values leaves the row usable: those values are not offered (options) and go back to the default (held)
    func blocker(_ s: Setting) -> Setting? {
        conflicting(s).first { $0.own == nil }?.other
    }
    func held(_ s: Setting) -> Bool {   // the row's value cannot be used now: the whole row is blocked, or its value is one another setting's value rules out
        let v = values[s.env] ?? s.def
        return conflicting(s).contains { c in c.own.map { $0.contains(v) } ?? !s.isDefault(v) }
    }
    func options(_ s: Setting) -> [[String]] {   // a choice's values on offer now: without the ones a conflict rules out, nor those below its atleast bound
        var o = s.options ?? []
        for c in conflicting(s) { if let own = c.own { o = o.filter { !own.contains($0[0]) } } }
        if let b = bound(s) { o = o.filter { (Double($0[0]) ?? .infinity) >= b - 1e-9 } }
        return o
    }
    // atleast: the number the named setting holds (nil: no such row, or a value that is not a number, such as game)
    func bound(_ s: Setting) -> Double? {
        guard let k = s.atleast, settings.contains(where: { $0.env == k }), let v = values[k] else { return nil }
        return Double(v)
    }
    // A value below the row's atleast bound goes up to the lowest value on offer (nil: not below it, or nothing on offer)
    private func raised(_ s: Setting) -> String? {
        guard let b = bound(s), let v = Double(values[s.env] ?? s.def), v < b - 1e-9 else { return nil }
        return options(s).first?[0]
    }
    @Published var raisedRows: Set<String> = []   // rows the launcher raised to their bound since the player last chose them: the row says so
    private func raise(_ s: Setting) {
        guard let w = raised(s) else { return }
        fputs("launcher: \(s.env)=\(values[s.env] ?? s.def) raised to \(w): not below \(s.atleast ?? "")=\(values[s.atleast ?? ""] ?? "")\n", stderr)
        values[s.env] = w; raisedRows.insert(s.env)
    }
    func set(_ s: Setting, _ v: String) {
        values[s.env] = v; raisedRows.remove(s.env)
        for r in settings where r.env != s.env && !fixed.contains(r.env) {
            if r.atleast == s.env { raise(r) }
            for c in conflicts(r) where c.key == s.env && c.want == v {
                if c.own == nil || c.own!.contains(values[r.env] ?? r.def) { values[r.env] = r.def }
            }
        }
    }

    var sections: [String] {
        settings.reduce(into: []) { if !$0.contains($1.sectionTitle) { $0.append($1.sectionTitle) } }
    }
    // A row with "needs" is a sub-setting of the row it names: drawn only while that setting has that value, so a
    // setting that cannot do anything is not offered (an upscaler's detail with the upscaler off, rumble strength
    // with rumble off). A row naming a setting this game does not have stays drawn, as it has nothing to hide behind
    // Sub-settings nest: a row is drawn only while the row it needs is drawn too (Softest needs Dynamic resolution, which needs Render resolution)
    func visible(_ s: Setting) -> Bool { visible(s, depth: 0) }
    private func visible(_ s: Setting, depth: Int) -> Bool {
        guard let n = s.needs, let eq = n.firstIndex(of: "=") else { return true }
        let key = String(n[n.startIndex..<eq]), want = n[n.index(after: eq)...].split(separator: "|").map(String.init)
        guard let v = values[key] else { return true }
        guard want.contains(v) else { return false }
        guard depth < 8, let parent = settings.first(where: { $0.env == key }) else { return true }
        return visible(parent, depth: depth + 1)
    }
    func rows(in section: String) -> [Setting] { settings.filter { $0.sectionTitle == section && visible($0) } }
    // The Mac window: Basic settings first, then Advanced settings, each in the list's sections and order
    func sections(basic: Bool) -> [String] {   // in the list's order of sections (each one's first row), so Advanced keeps Game first when its first rows are Basic
        sections.filter { sec in settings.contains { $0.sectionTitle == sec && $0.basic == basic } }
    }
    func rows(in section: String, basic: Bool) -> [Setting] { rows(in: section).filter { $0.basic == basic } }
    // A range's label: its percentage, with the size in pixels for the chosen console mode when the list has the base size (the render scale)
    func rangeLabel(_ s: Setting, _ v: Double) -> String {
        let p = "\(Int((v * 100).rounded())) %"
        guard let b = s.base, let size = size(b) else { return p }
        return "\(Self.pixels(Int((Double(size[0]) * v).rounded()), Int((Double(size[1]) * v).rounded()))) (\(p))"
    }
    // A base size for the chosen console mode: the first when entry whose setting holds, else the top level; nil without two positive numbers
    func size(_ b: Setting.Base) -> [Int]? {
        let handheld = (values["DM_OPMODE"] ?? "1") == "0"
        let alt = b.when?.first { a in
            guard let eq = a.is.firstIndex(of: "=") else { return false }
            return values[String(a.is[a.is.startIndex..<eq])] == String(a.is[a.is.index(after: eq)...])
        }
        let fullscreen = (values["DM_FULLSCREEN"] ?? "0") != "0"   // the host's reading: any value but 0 starts in fullscreen
        let size = alt.map { a in handheld ? a.handheld : a.display == true ? displaySizes[fullscreen] ?? a.docked : a.docked } ?? (handheld ? b.handheld : b.docked)
        return size.count == 2 && size.allSatisfy({ $0 > 0 }) ? size : nil
    }
    static func pixels(_ w: Int, _ h: Int) -> String { "\(w) × \(h)" }
    // An upscale rate's render size (rate_base): the output for the console mode and output resolution chosen, divided by the rate, rounded up as the game does
    func renderSize(_ s: Setting, rate: String) -> [Int]? {
        guard let b = s.rateBase, let r = s.rate(rate), r >= 1, let size = size(b) else { return nil }
        return size.map { Int((Double($0) / r).rounded(.up)) }
    }
    // A choice's option as shown: its label, and its render size in pixels when the row's options are upscale rates ("Quality (1.5) · 1920 × 1080")
    func optionLabel(_ s: Setting, _ o: [String]) -> String {
        guard let px = renderSize(s, rate: o[0]) else { return o[1] }
        return "\(o[1]) · \(Self.pixels(px[0], px[1]))"
    }
    // The line under a section of upscale rates: the render size of its first drawn rate row (when its value is a number) down to that of a drawn row it bounds (atleast),
    // and the output they are upscaled to. nil without such a row, or at the game's own rate
    func renderSummary(in section: String) -> String? {
        let drawn = rows(in: section)
        guard let top = drawn.first(where: { $0.rateBase != nil }), let b = top.rateBase, let out = size(b),
              let r0 = top.rate(values[top.env] ?? top.def), let a = renderSize(top, rate: values[top.env] ?? top.def) else { return nil }
        let aa = top.aliased(values[top.env] ?? top.def)   // a rates value: its upscaler runs at the output's size, as anti-aliasing
        let low = drawn.first { $0.atleast == top.env && $0.rateBase != nil }.flatMap { renderSize($0, rate: values[$0.env] ?? $0.def) }.flatMap { $0 == a ? nil : $0 }
        let range = Self.pixels(a[0], a[1]) + (low.map { " to " + Self.pixels($0[0], $0[1]) } ?? "")
        if r0 <= 1 && low == nil { return "Renders \(range), the output itself: " + (aa ? "the upscaler anti-aliases it." : "no upscaler.") }
        return "Renders \(range), upscaled to \(Self.pixels(out[0], out[1]))." + (r0 <= 1 ? " At \(Self.pixels(a[0], a[1])) " + (aa ? "the upscaler anti-aliases the output itself." : "the game's upscaler is off.") : "")
    }
    var changed: Bool { settings.contains { !fixed.contains($0.env) && !$0.isDefault(values[$0.env] ?? $0.def) } }

    // Reset every launcher choice to the game's defaults on the screen; values fixed by an env file or at launch stay. The saved choices are forgotten when Play applies
    // the defaults (apply() keeps no default), so Quit after Reset keeps them, and a choice a launch override hides is kept
    func reset() {
        for s in settings where !fixed.contains(s.env) { values[s.env] = s.def }
        fputs("launcher: settings reset to the game's defaults\n", stderr)
    }

    // Before the game starts: every setting not set by an env file goes into the environment (DM_LANG replaces the packed language main.m set as a default). Only a choice that
    // differs from the game's default is stored: a setting the player left at its default follows the defaults of a later build (the settings it was tested with)
    func apply() {
        for s in settings {
            let v = values[s.env] ?? s.def
            if fixed.contains(s.env) { fputs("launcher: \(s.env) kept at \(v) (\(fromLaunch.contains(s.env) ? "launch environment" : "env file"))\n", stderr); continue }
            let w = held(s) ? s.def : raised(s) ?? v   // held at its default while a conflicting value holds (one an env file fixes); raised to its atleast bound
            store(s, w)
            setenv(s.env, w, 1); fputs("launcher: \(s.env)=\(w)\(s.isDefault(w) ? "" : " (changed)")\(w == v ? "" : held(s) ? " (held: conflicts with \(conflicting(s).first?.other.env ?? ""))" : " (raised: not below \(s.atleast ?? ""))")\n", stderr)
        }
    }
    private func store(_ s: Setting, _ v: String) {
        if !ignoreSaved { if s.isDefault(v) { defaults.removeObject(forKey: "launcher." + s.env) } else { defaults.set(v, forKey: "launcher." + s.env) } }
    }

    // A choice made while the game runs (the iOS in-game menu, GameMenu.swift): saved as Play saves it, so the settings screen shows it at the next start, and put in the environment.
    // A value an env file or the launch fixed stays
    func choose(_ s: Setting, _ v: String) {
        guard !fixed.contains(s.env), s.accepts(v) else { return }
        guard blocker(s) == nil, s.kind != "choice" || options(s).contains(where: { $0[0] == v }) else { return }   // a value a conflict rules out now is not taken
        set(s, v); store(s, v); setenv(s.env, v, 1)
        fputs("launcher: \(s.env)=\(v) (in-game menu)\n", stderr)
    }

    // DM_LAUNCHER=0: no screen, but the saved choices still apply (only DM_SAVED_CHOICES=0 ignores them); nothing is written
    func applySaved() {
        for s in settings where !fixed.contains(s.env) {
            let v = values[s.env] ?? s.def
            if !s.isDefault(v) { setenv(s.env, v, 1); fputs("launcher: \(s.env)=\(v) (saved choice)\n", stderr) }
        }
    }
}
