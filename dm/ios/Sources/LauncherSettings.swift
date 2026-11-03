// Launcher settings, validation and persistence. Shared by the iOS UI, the Mac app's settings window (dm/mac/Launcher.swift) and the native storage regression test.
import Foundation
import Combine

struct Setting: Decodable, Identifiable {
    let env, kind, label, note, def: String
    let section: String?   // heading on the settings screen; absent in older bundles
    let needs: String?     // "<VAR>=<value>[|<value>...]": a sub-setting, drawn only while that setting has one of the values
    let conflicts: String? // "<VAR>=<value>": held at its default, and drawn disabled, while that setting has that value; "<VAR>=<value>:<own>[|<own>...]": only those values of this row
    let level: String?     // "basic": shown first (the Mac window's Basic settings); any other or none: Advanced settings. The iPhone keeps one list
    let base: Base?        // render scale: the 3D views' size at 1.0, docked and handheld, for the size in pixels (the Mac window)
    struct Base: Decodable { let docked, handheld: [Int] }
    let options: [[String]]?   // choice: [value, label]
    let min, max, step: Double?   // range
    let on, off: String?   // toggle: the values the variable takes (1 / 0 when not given)
    var id: String { env }
    var sectionTitle: String { section ?? "Game" }
    var basic: Bool { level == "basic" }
    var onValue: String { on ?? "1" }
    var offValue: String { off ?? "0" }
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
    enum CodingKeys: String, CodingKey { case env, kind, label, note, def = "default", section, needs, conflicts, level, base, options, min, max, step, on, off }
}
struct SettingsFile: Decodable { let settings: [Setting] }

final class LauncherModel: ObservableObject {
    let settings: [Setting]
    let fixed: Set<String>      // set by an env file or by the launch environment: shown, not changed
    let fromLaunch: Set<String>   // of those, the ones the launch environment set (a front end's launch overrides, Xcode; DM_ENV_LAUNCH_KEYS from launcher.c or main.m)
    let ignoreSaved: Bool   // DM_SAVED_CHOICES=0: start from the defaults, and leave the saved choices as they are
    @Published var values: [String: String] = [:]

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
    }

    // A row's "conflicts" as its parts: the setting, its value, and this row's own values that cannot go with it (nil: the whole row)
    private func conflict(_ s: Setting) -> (key: String, want: String, own: [String]?)? {
        guard let c = s.conflicts, let eq = c.firstIndex(of: "=") else { return nil }
        let rest = c[c.index(after: eq)...], colon = rest.firstIndex(of: ":")
        let want = String(colon.map { rest[rest.startIndex..<$0] } ?? rest)
        let own = colon.map { rest[rest.index(after: $0)...].split(separator: "|").map(String.init) }
        return (String(c[c.startIndex..<eq]), want, own)
    }
    // The other row while it has the value this row conflicts with (drawn, so its value counts), nil otherwise
    private func conflicting(_ s: Setting) -> (other: Setting, own: [String]?)? {
        guard let c = conflict(s), values[c.key] == c.want, let other = settings.first(where: { $0.env == c.key }), visible(other) else { return nil }
        return (other, c.own)
    }
    // A row with "conflicts" cannot be used together with that setting's value: while the other row has it, this one is held at
    // its default and drawn disabled. Choosing a value resets the rows that conflict with it, so the two never hold together; a value an env file fixes stays.
    // A conflict of some of the row's own values leaves the row usable: those values are not offered (options) and go back to the default (held)
    func blocker(_ s: Setting) -> Setting? {
        guard let c = conflicting(s), c.own == nil else { return nil }
        return c.other
    }
    func held(_ s: Setting) -> Bool {   // the row's value cannot be used now: the whole row is blocked, or its value is one the other setting's value rules out
        guard let c = conflicting(s) else { return false }
        guard let own = c.own else { return !s.isDefault(values[s.env] ?? s.def) }
        return own.contains(values[s.env] ?? s.def)
    }
    func options(_ s: Setting) -> [[String]] {   // a choice's values on offer now: without the ones a conflict rules out
        guard let c = conflicting(s), let own = c.own else { return s.options ?? [] }
        return (s.options ?? []).filter { !own.contains($0[0]) }
    }
    func set(_ s: Setting, _ v: String) {
        values[s.env] = v
        for r in settings where r.env != s.env && !fixed.contains(r.env) {
            guard let c = conflict(r), c.key == s.env, c.want == v else { continue }
            if c.own == nil || c.own!.contains(values[r.env] ?? r.def) { values[r.env] = r.def }
        }
    }

    var sections: [String] {
        settings.reduce(into: []) { if !$0.contains($1.sectionTitle) { $0.append($1.sectionTitle) } }
    }
    // A row with "needs" is a sub-setting of the row it names: drawn only while that setting has that value, so a
    // setting that cannot do anything is not offered (an upscaler's detail with the upscaler off, rumble strength
    // with rumble off). A row naming a setting this game does not have stays drawn, as it has nothing to hide behind
    func visible(_ s: Setting) -> Bool {
        guard let n = s.needs, let eq = n.firstIndex(of: "=") else { return true }
        let key = String(n[n.startIndex..<eq]), want = n[n.index(after: eq)...].split(separator: "|").map(String.init)
        guard let v = values[key] else { return true }
        return want.contains(v)
    }
    func rows(in section: String) -> [Setting] { settings.filter { $0.sectionTitle == section && visible($0) } }
    // The Mac window: Basic settings first, then Advanced settings, each in the list's sections and order
    func sections(basic: Bool) -> [String] {
        settings.filter { $0.basic == basic }.reduce(into: []) { if !$0.contains($1.sectionTitle) { $0.append($1.sectionTitle) } }
    }
    func rows(in section: String, basic: Bool) -> [Setting] { rows(in: section).filter { $0.basic == basic } }
    // A range's label: its percentage, with the size in pixels for the chosen console mode when the list has the base size (the render scale)
    func rangeLabel(_ s: Setting, _ v: Double) -> String {
        let p = "\(Int((v * 100).rounded())) %"
        guard let b = s.base else { return p }
        let size = (values["DM_OPMODE"] ?? "1") == "0" ? b.handheld : b.docked
        guard size.count == 2 else { return p }
        return "\(Int((Double(size[0]) * v).rounded())) × \(Int((Double(size[1]) * v).rounded())) (\(p))"
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
            let w = held(s) ? s.def : v   // held at its default while a conflicting value holds (one an env file fixes)
            store(s, w)
            setenv(s.env, w, 1); fputs("launcher: \(s.env)=\(w)\(s.isDefault(w) ? "" : " (changed)")\(w != v ? " (held: conflicts with \(conflicting(s)?.other.env ?? ""))" : "")\n", stderr)
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
