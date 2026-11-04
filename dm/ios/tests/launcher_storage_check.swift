// The launcher's storage (LauncherSettings.swift LauncherModel), run by test_launcher_storage.py on macOS: only choices that differ from the game's default are
// stored, Reset to tested defaults followed by Play forgets the choices, an env file's values are kept. Arguments: the iOS and the Mac launcher.json of Breath of the Wild (settings.py, GFXAPI=nvn)
import Foundation
// launcher.c's Match display size, which dm/mac/Launcher.swift calls: launcher.c is not linked into the check (it has its own main), and no screen is measured here
@_cdecl("dm_mac_match_display") func dmMacMatchDisplayStub(_ fullscreen: Int32, _ w: UnsafeMutablePointer<Int32>, _ h: UnsafeMutablePointer<Int32>) -> Int32 { 0 }
@main struct Check {
    static func main() {
        var failed = 0
        func expect(_ c: Bool, _ what: String) { print((c ? "PASS " : "FAIL ") + what); if !c { failed += 1 } }
        let file = try! JSONDecoder().decode(SettingsFile.self, from: Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[1])))
        let suite = "nxnative.launcher-test." + UUID().uuidString
        let d = UserDefaults(suiteName: suite)!
        defer { d.removePersistentDomain(forName: suite) }
        d.set("1", forKey: "launcher.DM_PRESENT_SNAP")   // stored by the old launcher, equal to the default
        d.set("30", forKey: "launcher.DM_FPS_TARGET")    // a changed game setting
        d.set("0", forKey: "launcher.DM_PSO_MTL4")       // a changed performance setting
        d.set("1", forKey: "launcher.DM_FPS_HOLD")       // a value the toggle does not take (on / off): the default
        setenv("DM_ENV_FILE_KEYS", "DM_METAL_HUD", 1); setenv("DM_METAL_HUD", "1", 1)   // set by Documents/env.txt
        let m = LauncherModel(file.settings, defaults: d)
        let scale = file.settings.first { $0.env == "DM_RENDER_SCALE" }!
        expect(["nan", "inf", "-inf", "0.49", "1.01", "invalid", "0x1p-1", "0X1", " 0.75"].allSatisfy { !scale.accepts($0) } && scale.accepts("0.5") && scale.accepts("1.00"), "saved ranges reject nonfinite, out-of-range, hex and padded values, as launcher.c does")
        d.set("nan", forKey: "launcher.DM_RENDER_SCALE")
        expect(LauncherModel(file.settings, defaults: d).values["DM_RENDER_SCALE"] == scale.def, "an invalid saved range loads the default")
        expect(m.values["DM_FPS_TARGET"] == "30" && m.values["DM_PSO_MTL4"] == "0" && m.values["DM_FPS_HOLD"] == "on" && m.values["DM_METAL_HUD"] == "1", "stored choices and the env file are read")
        expect(m.changed, "Reset is offered with changed settings")
        m.values["DM_RENDER_SCALE"] = "1.00"   // the slider's format of the default
        m.apply()
        expect(d.string(forKey: "launcher.DM_PRESENT_SNAP") == nil, "a value equal to the default is not stored (and an old one is dropped)")
        expect(d.string(forKey: "launcher.DM_RENDER_SCALE") == nil && d.string(forKey: "launcher.DM_OPMODE") == nil && d.string(forKey: "launcher.DM_FPS_HOLD") == nil, "defaults are not stored")
        expect(d.string(forKey: "launcher.DM_FPS_TARGET") == "30" && d.string(forKey: "launcher.DM_PSO_MTL4") == "0", "changed choices are stored")
        expect(d.string(forKey: "launcher.DM_METAL_HUD") == nil && String(cString: getenv("DM_METAL_HUD")) == "1", "the env file's value is kept and not stored")
        expect(String(cString: getenv("DM_PSO_MTL4")) == "0" && String(cString: getenv("DM_PRESENT_SNAP")) == "1" && String(cString: getenv("DM_FPS_HOLD")) == "on", "the environment gets every value")
        m.reset()
        expect(m.values["DM_PSO_MTL4"] == "1" && m.values["DM_FPS_TARGET"] == file.settings.first { $0.env == "DM_FPS_TARGET" }!.def, "Reset shows the defaults")
        expect(d.string(forKey: "launcher.DM_PSO_MTL4") == "0" && d.string(forKey: "launcher.DM_FPS_TARGET") == "30", "Reset alone forgets nothing: Quit after it keeps the choices")
        expect(m.values["DM_METAL_HUD"] == "1" && !m.changed, "Reset leaves the env file's value; nothing left to reset")
        m.apply()
        expect(d.string(forKey: "launcher.DM_PSO_MTL4") == nil && d.string(forKey: "launcher.DM_FPS_TARGET") == nil, "Play after Reset forgets the choices")
        d.set(30, forKey: "launcher.DM_FPS_TARGET"); d.set(false, forKey: "launcher.DM_RUMBLE")   // a number and a boolean, as defaults write -int / -bool stores them
        let typed = LauncherModel(file.settings, defaults: d)
        expect(typed.values["DM_FPS_TARGET"] == file.settings.first { $0.env == "DM_FPS_TARGET" }!.def && typed.values["DM_RUMBLE"] == "1", "a saved value that is not a string is ignored, as launcher.c ignores it")
        d.removeObject(forKey: "launcher.DM_FPS_TARGET"); d.removeObject(forKey: "launcher.DM_RUMBLE")
        // Sub-settings: a row with "needs" is drawn only while the setting it names has that value
        m.values["DM_RUMBLE"] = "1"
        expect(m.rows(in: "Controls").contains { $0.env == "DM_RUMBLE_GAIN" }, "a sub-setting is drawn while its setting is on")
        m.values["DM_RUMBLE"] = "0"
        expect(!m.rows(in: "Controls").contains { $0.env == "DM_RUMBLE_GAIN" }, "a sub-setting is hidden while its setting is off")
        expect(m.rows(in: "Controls").contains { $0.env == "DM_RUMBLE" }, "the setting it depends on stays drawn")
        m.values["DM_RUMBLE"] = "1"
        // DM_LAUNCHER=0 skips the screen, not the saved choices; DM_SAVED_CHOICES=0 ignores them and leaves them stored
        let fps = file.settings.first { $0.env == "DM_FPS_TARGET" }!
        d.set("30", forKey: "launcher.DM_FPS_TARGET"); unsetenv("DM_FPS_TARGET")
        LauncherModel(file.settings, defaults: d).applySaved()
        expect(String(cString: getenv("DM_FPS_TARGET")) == "30" && d.string(forKey: "launcher.DM_FPS_TARGET") == "30", "without the screen, a saved choice goes into the environment and stays saved")
        setenv("DM_SAVED_CHOICES", "0", 1); unsetenv("DM_FPS_TARGET")
        let plain = LauncherModel(file.settings, defaults: d)
        expect(plain.values["DM_FPS_TARGET"] == fps.def, "DM_SAVED_CHOICES=0 starts from the defaults")
        plain.applySaved()
        expect(getenv("DM_FPS_TARGET") == nil, "DM_SAVED_CHOICES=0: nothing saved goes into the environment")
        plain.apply()
        expect(String(cString: getenv("DM_FPS_TARGET")) == fps.def && d.string(forKey: "launcher.DM_FPS_TARGET") == "30", "DM_SAVED_CHOICES=0: Play uses the screen's values and leaves the saved choices")
        plain.reset()
        expect(d.string(forKey: "launcher.DM_FPS_TARGET") == "30", "DM_SAVED_CHOICES=0: Reset leaves the saved choices too")
        unsetenv("DM_SAVED_CHOICES")
        // The Mac window's list (second argument): Basic first, then Advanced in the list's sections; Display mode; the render scale in pixels; values set at launch are fixed
        let mac = try! JSONDecoder().decode(SettingsFile.self, from: Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[2])))
        setenv("DM_ENV_FILE_KEYS", "", 1); unsetenv("DM_FULLSCREEN"); unsetenv("DM_OPMODE")
        let mm = LauncherModel(mac.settings, defaults: d)
        expect(mm.sections(basic: true) == ["Game", "Controls"] && mm.sections(basic: false) == ["Game", "Resolution", "Effects", "HDR", "Controls", "Performance"],
               "Basic keeps its sections; Advanced has the picture's (Upscaling under Resolution, filtering under Effects, HDR), in the list's order: \(mm.sections(basic: false))")
        expect(mm.rows(in: "Game", basic: true).map(\.env) == ["DM_FPS_TARGET", "DM_RENDER_SCALE", "DM_OPMODE", "DM_LANG", "DM_FULLSCREEN"], "Basic rows in the list's order")
        expect(!file.settings.contains { $0.env == "DM_FULLSCREEN" } && mm.values["DM_FULLSCREEN"] == "0", "Display mode is the Mac's, Window by default")
        mm.values["DM_FULLSCREEN"] = "1"; mm.apply()
        expect(String(cString: getenv("DM_FULLSCREEN")) == "1" && d.string(forKey: "launcher.DM_FULLSCREEN") == "1", "Fullscreen goes into the environment and is saved")
        mm.values["DM_FULLSCREEN"] = "0"; mm.apply()
        expect(String(cString: getenv("DM_FULLSCREEN")) == "0" && d.string(forKey: "launcher.DM_FULLSCREEN") == nil, "Window is the default: set to 0, not saved")
        let macScale = mac.settings.first { $0.env == "DM_RENDER_SCALE" }!
        mm.values["DM_OPMODE"] = "1"
        expect(mm.rangeLabel(macScale, 1) == "1600 × 900 (100 %)" && mm.rangeLabel(macScale, 0.5) == "800 × 450 (50 %)", "docked size in pixels at 1.0 and 0.5")
        mm.values["DM_OPMODE"] = "0"
        expect(mm.rangeLabel(macScale, 0.75) == "960 × 540 (75 %)", "handheld size in pixels")
        let plainScale = try! JSONDecoder().decode(Setting.self, from: Data(#"{"env":"DM_X","kind":"range","label":"X","note":"","default":"1.0","min":0.5,"max":1.0,"step":0.05,"level":"expert"}"#.utf8))
        expect(mm.rangeLabel(plainScale, 0.5) == "50 %" && !plainScale.basic, "no base size: the percentage; an unknown level decodes as Advanced")
        d.set("1", forKey: "launcher.DM_OPMODE"); setenv("DM_ENV_LAUNCH_KEYS", "DM_OPMODE", 1); setenv("DM_OPMODE", "0", 1)   // a front end's launch override over a saved choice
        let launched = LauncherModel(mac.settings, defaults: d)
        expect(launched.values["DM_OPMODE"] == "0" && launched.fixed.contains("DM_OPMODE") && launched.fromLaunch.contains("DM_OPMODE"), "a variable set at launch is shown fixed, over the saved choice")
        launched.apply()
        expect(String(cString: getenv("DM_OPMODE")) == "0" && d.string(forKey: "launcher.DM_OPMODE") == "1", "Play keeps the launch value and leaves the saved choice")
        launched.reset(); launched.apply()
        expect(launched.values["DM_OPMODE"] == "0" && d.string(forKey: "launcher.DM_OPMODE") == "1", "Reset then Play keeps the saved choice of a setting fixed at launch")
        unsetenv("DM_OPMODE"); setenv("DM_ENV_LAUNCH_KEYS", "", 1); d.removeObject(forKey: "launcher.DM_OPMODE")
        // A conflict of some of a row's own values (conflicts=<VAR>=<value>:<own>|...) and a sub-setting of several values (needs=<VAR>=<a>|<b>), as Donkey Kong Bananza's
        // upscaler quality (Ultra performance only with a MetalFX upscaler) and its dynamic resolution (only with a fixed quality)
        let rows = try! JSONDecoder().decode(SettingsFile.self, from: Data(#"""
            {"settings": [{"env":"DM_T_UP","kind":"choice","label":"Upscaler","note":"","default":"game","options":[["game","Game"],["metalfx","MetalFX"]]},
                          {"env":"DM_T_Q","kind":"choice","label":"Quality","note":"","default":"game","conflicts":"DM_T_UP=game:3.0","options":[["game","Game"],["1.5","Q"],["3.0","U"]]},
                          {"env":"DM_T_DYN","kind":"toggle","label":"Dynamic","note":"","default":"0","needs":"DM_T_Q=1.0|1.5"}]}
            """#.utf8)).settings
        d.set("3.0", forKey: "launcher.DM_T_Q")   // saved with MetalFX, then MetalFX unset: the saved value goes back to the default
        let vc = LauncherModel(rows, defaults: d)
        let q = rows[1], dyn = rows[2]
        expect(vc.values["DM_T_Q"] == "game" && vc.blocker(q) == nil, "a saved value its conflict rules out loads as the default; the row itself stays usable")
        expect(vc.options(q).map { $0[0] } == ["game", "1.5"], "while the other setting has the value, the conflicting values are not offered")
        vc.set(rows[0], "metalfx"); vc.set(q, "3.0")
        expect(vc.options(q).count == 3 && !vc.held(q), "with the other value, every value is offered and taken")
        vc.set(rows[0], "game")
        expect(vc.values["DM_T_Q"] == "game", "choosing the conflicting value resets a row that holds one of its values")
        vc.set(q, "1.5"); vc.set(rows[0], "metalfx"); vc.set(rows[0], "game")
        expect(vc.values["DM_T_Q"] == "1.5", "a value outside the conflict is kept")
        expect(vc.visible(dyn), "a sub-setting is drawn while its setting has one of the listed values")
        vc.set(q, "game")
        expect(!vc.visible(dyn), "and hidden at a value outside the list")
        vc.values["DM_T_Q"] = "3.0"; vc.apply()
        expect(String(cString: getenv("DM_T_Q")) == "game", "Play holds a value the conflict rules out at the default")
        for k in ["DM_T_UP", "DM_T_Q", "DM_T_DYN"] { unsetenv(k); d.removeObject(forKey: "launcher." + k) }
        // Donkey Kong Bananza's Resolution section: render sizes of upscale rates (rate_base, with the output for Output resolution: Window), nested sub-settings
        // (Softest needs Dynamic resolution, which needs Render resolution), and a softest rate never sharper than the render resolution (atleast)
        let res = try! JSONDecoder().decode(SettingsFile.self, from: Data(#"""
            {"settings": [{"env":"DM_OPMODE","kind":"choice","section":"Game","label":"Console mode","note":"","default":"1","options":[["1","Docked"],["0","Handheld"]],"level":"basic"},
                          {"env":"DM_SINGLE_UPSCALE","kind":"choice","section":"Resolution","label":"Output resolution","note":"","default":"0","options":[["0","Game default"],["1","Window"],["display","Match display"]]},
                          {"env":"DM_UPSCALER","kind":"choice","section":"Resolution","label":"Game's upscaler","note":"","default":"game","options":[["game","Game's own"],["metalfx-temporal","MetalFX temporal"]]},
                          {"env":"DM_UPSCALE_RATIO","kind":"choice","section":"Resolution","label":"Render resolution","note":"","default":"game","conflicts":"DM_UPSCALER=game:3.0",
                           "options":[["game","Game default (dynamic)"],["1.0","Native (1.0, no upscaler)"],["1.5","Quality (1.5)"],["1.6875","Balanced (1.69)"],["2.0","Performance (2.0)"],["3.0","Ultra performance (3.0, MetalFX only)"]],
                           "rate_base":{"docked":[2304,1296],"handheld":[1920,1080],"when":[{"is":"DM_SINGLE_UPSCALE=1","docked":[2880,1620],"handheld":[1920,1080]},{"is":"DM_SINGLE_UPSCALE=display","docked":[0,0],"handheld":[1920,1080],"display":true}]}},
                          {"env":"DM_UPSCALE_DYNAMIC","kind":"toggle","section":"Resolution","label":"Dynamic resolution","note":"","default":"0","on":"1","off":"0","needs":"DM_UPSCALE_RATIO=1.0|1.5|1.6875|2.0"},
                          {"env":"DM_UPSCALE_SOFTEST","kind":"choice","section":"Resolution","label":"Softest","note":"","default":"2.0","needs":"DM_UPSCALE_DYNAMIC=1","atleast":"DM_UPSCALE_RATIO","conflicts":"DM_UPSCALER=game:3.0",
                           "options":[["1.0","Native (1.0, no upscaler)"],["1.5","Quality (1.5)"],["1.6875","Balanced (1.69)"],["2.0","Performance (2.0)"],["3.0","Ultra performance (3.0, MetalFX only)"]],
                           "rate_base":{"docked":[2304,1296],"handheld":[1920,1080],"when":[{"is":"DM_SINGLE_UPSCALE=1","docked":[2880,1620],"handheld":[1920,1080]},{"is":"DM_SINGLE_UPSCALE=display","docked":[0,0],"handheld":[1920,1080],"display":true}]}},
                          {"env":"DM_EFFECTS_RES","kind":"choice","section":"Effects","label":"Effects resolution","note":"","default":"game","options":[["game","Game default (half)"],["full","Full"]]}]}
            """#.utf8)).settings
        let rm = LauncherModel(res, defaults: d)
        let ratio = res[3], dynRow = res[4], soft = res[5]
        expect(rm.sections(basic: false) == ["Resolution", "Effects"] && rm.sections(basic: true) == ["Game"], "the Mac window's Advanced sections in the list's order")
        expect(rm.optionLabel(ratio, ["1.5", "Quality (1.5)"]) == "Quality (1.5) · 1536 × 864" && rm.optionLabel(ratio, ["game", "Game default (dynamic)"]) == "Game default (dynamic)",
               "a rate's render size for the game's docked output; the game's own has none")
        rm.set(res[1], "1")
        expect(rm.optionLabel(ratio, ["1.5", "Quality (1.5)"]) == "Quality (1.5) · 1920 × 1080" && rm.optionLabel(ratio, ["1.6875", "B"]) == "B · 1707 × 960", "Output resolution: Window: the window's size divided, rounded up")
        rm.values["DM_OPMODE"] = "0"
        expect(rm.optionLabel(ratio, ["2.0", "P"]) == "P · 960 × 540", "handheld: its own output")
        rm.values["DM_OPMODE"] = "1"
        // Output resolution: Match display: the screen's size, which only a front end that measures the screen knows (the Mac window, launcher.c)
        rm.set(res[1], "display")
        expect(rm.optionLabel(ratio, ["1.5", "Quality (1.5)"]) == "Quality (1.5)", "Match display without the screen's size (iOS, an older front end): no render size")
        rm.displaySizes = [false: [1408, 792], true: [2560, 1440]]
        expect(rm.optionLabel(ratio, ["1.5", "Q"]) == "Q · 939 × 528", "Match display in a window: the window's size divided")
        rm.values["DM_FULLSCREEN"] = "1"
        expect(rm.optionLabel(ratio, ["1.6875", "B"]) == "B · 1518 × 854", "Match display fullscreen: the screen's 2560 × 1440 divided")
        rm.values["DM_OPMODE"] = "0"
        expect(rm.optionLabel(ratio, ["2.0", "P"]) == "P · 960 × 540", "Match display handheld: the game's handheld output")
        rm.values["DM_OPMODE"] = "1"; rm.values["DM_FULLSCREEN"] = nil; rm.displaySizes = [:]
        rm.set(res[1], "1")
        expect(!rm.visible(dynRow) && !rm.visible(soft) && rm.renderSummary(in: "Resolution") == nil, "the game's own rate: no dynamic rows, no summary")
        rm.set(ratio, "1.5"); rm.set(dynRow, "1")
        expect(rm.visible(dynRow) && rm.visible(soft), "a rate: Dynamic resolution, and with it on, Softest")
        expect(rm.renderSummary(in: "Resolution") == "Renders 1920 × 1080 to 1440 × 810, upscaled to 2880 × 1620.", "the summary names both sizes: \(rm.renderSummary(in: "Resolution") ?? "nil")")
        expect(rm.options(soft).map { $0[0] } == ["1.5", "1.6875", "2.0"], "Softest offers nothing sharper than the render resolution (and no Ultra with the game's upscaler)")
        rm.set(soft, "1.6875"); rm.set(ratio, "2.0")
        expect(rm.values["DM_UPSCALE_SOFTEST"] == "2.0" && rm.raisedRows.contains("DM_UPSCALE_SOFTEST"), "a sharper render resolution keeps Softest; a softer one raises it, and the row says so")
        rm.set(soft, "2.0")
        expect(!rm.raisedRows.contains("DM_UPSCALE_SOFTEST"), "choosing the row again clears the note")
        rm.set(ratio, "game")
        expect(!rm.visible(dynRow) && !rm.visible(soft) && rm.options(soft).count == 4, "the game's own rate: Softest hides with Dynamic resolution (nested), and its bound goes")
        rm.set(ratio, "1.0")
        expect(rm.renderSummary(in: "Resolution") == "Renders 2880 × 1620 to 1440 × 810, upscaled to 2880 × 1620. At 2880 × 1620 the game's upscaler is off.", "a Native sharpest: the upscaler is off there")
        rm.set(dynRow, "0")
        expect(rm.renderSummary(in: "Resolution") == "Renders 2880 × 1620, the output itself: no upscaler." && !rm.visible(soft), "Native fixed: no upscaler")
        d.set("1.5", forKey: "launcher.DM_UPSCALE_SOFTEST"); d.set("1.6875", forKey: "launcher.DM_UPSCALE_RATIO")   // a pair saved by a front end that ignores atleast
        let saved = LauncherModel(res, defaults: d)
        expect(saved.values["DM_UPSCALE_SOFTEST"] == "1.6875", "a saved softest rate below the saved render resolution loads raised")
        saved.values["DM_UPSCALE_SOFTEST"] = "1.0"; saved.apply()
        expect(String(cString: getenv("DM_UPSCALE_SOFTEST")) == "1.6875", "Play never sends a softest rate below the render resolution")
        for s in res { unsetenv(s.env); d.removeObject(forKey: "launcher." + s.env) }
        // Native (MetalFX AA): a rate value that is not a number (rates=native-mfx:1, the output's size), offered only with MetalFX temporal (two conflict clauses joined by ","),
        // and Softest without Native while it is the sharpest (a second clause on another setting)
        let aa = try! JSONDecoder().decode(SettingsFile.self, from: Data(#"""
            {"settings": [{"env":"DM_SINGLE_UPSCALE","kind":"choice","section":"Resolution","label":"Output resolution","note":"","default":"1","options":[["0","Game default"],["1","Window"]]},
                          {"env":"DM_UPSCALER","kind":"choice","section":"Resolution","label":"Game's upscaler","note":"","default":"game","options":[["game","Game's own"],["metalfx","MetalFX"],["metalfx-temporal","MetalFX temporal"]]},
                          {"env":"DM_UPSCALE_RATIO","kind":"choice","section":"Resolution","label":"Render resolution","note":"","default":"game","conflicts":"DM_UPSCALER=game:3.0|native-mfx,DM_UPSCALER=metalfx:native-mfx","rates":"native-mfx:1",
                           "options":[["game","Game default (dynamic)"],["1.0","Native (1.0, no upscaler)"],["native-mfx","Native (MetalFX AA)"],["1.5","Quality (1.5)"],["2.0","Performance (2.0)"],["3.0","Ultra performance (3.0, MetalFX only)"]],
                           "rate_base":{"docked":[2304,1296],"handheld":[1920,1080],"when":[{"is":"DM_SINGLE_UPSCALE=1","docked":[2880,1620],"handheld":[1920,1080]}]}},
                          {"env":"DM_UPSCALE_DYNAMIC","kind":"toggle","section":"Resolution","label":"Dynamic resolution","note":"","default":"0","on":"1","off":"0","needs":"DM_UPSCALE_RATIO=1.0|native-mfx|1.5|2.0"},
                          {"env":"DM_UPSCALE_SOFTEST","kind":"choice","section":"Resolution","label":"Softest","note":"","default":"2.0","needs":"DM_UPSCALE_DYNAMIC=1","atleast":"DM_UPSCALE_RATIO","conflicts":"DM_UPSCALER=game:3.0,DM_UPSCALE_RATIO=native-mfx:1.0",
                           "options":[["1.0","Native (1.0, no upscaler)"],["1.5","Quality (1.5)"],["2.0","Performance (2.0)"],["3.0","Ultra performance (3.0, MetalFX only)"]],
                           "rate_base":{"docked":[2304,1296],"handheld":[1920,1080],"when":[{"is":"DM_SINGLE_UPSCALE=1","docked":[2880,1620],"handheld":[1920,1080]}]}}]}
            """#.utf8)).settings
        let am = LauncherModel(aa, defaults: d)
        let up = aa[1], ar = aa[2], ad = aa[3], asoft = aa[4]
        expect(am.options(ar).map { $0[0] } == ["game", "1.0", "1.5", "2.0"], "the game's upscaler: neither Ultra performance nor Native (MetalFX AA)")
        am.set(up, "metalfx")
        expect(am.options(ar).map { $0[0] } == ["game", "1.0", "1.5", "2.0", "3.0"], "MetalFX's spatial scaler: Ultra performance, not Native (MetalFX AA) (the second clause)")
        am.set(up, "metalfx-temporal")
        expect(am.options(ar).count == 6, "MetalFX temporal: every value")
        am.set(ar, "native-mfx")
        expect(am.optionLabel(ar, ["native-mfx", "Native (MetalFX AA)"]) == "Native (MetalFX AA) · 2880 × 1620", "Native (MetalFX AA) shows the output's size (rates)")
        expect(am.renderSummary(in: "Resolution") == "Renders 2880 × 1620, the output itself: the upscaler anti-aliases it.", "its summary: \(am.renderSummary(in: "Resolution") ?? "nil")")
        am.set(asoft, "1.0"); am.set(ad, "1")
        expect(am.visible(ad) && am.options(asoft).map { $0[0] } == ["1.5", "2.0", "3.0"], "as the sharpest of a dynamic range: Softest starts at Quality")
        expect(am.held(asoft) && am.values["DM_UPSCALE_SOFTEST"] == "1.0", "a Native softest held from before is held at the default while Native (MetalFX AA) is the sharpest")
        am.set(asoft, "2.0")
        expect(am.renderSummary(in: "Resolution") == "Renders 2880 × 1620 to 1440 × 810, upscaled to 2880 × 1620. At 2880 × 1620 the upscaler anti-aliases the output itself.", "the dynamic summary: \(am.renderSummary(in: "Resolution") ?? "nil")")
        am.set(asoft, "1.5"); am.set(ar, "1.0"); am.set(asoft, "1.0"); am.set(ar, "native-mfx")
        expect(am.values["DM_UPSCALE_SOFTEST"] == "2.0", "choosing Native (MetalFX AA) resets a Native softest to its default")
        am.set(up, "metalfx")
        expect(am.values["DM_UPSCALE_RATIO"] == "game", "choosing the spatial scaler resets Native (MetalFX AA) (the second clause)")
        am.values["DM_UPSCALE_RATIO"] = "native-mfx"; am.apply()
        expect(String(cString: getenv("DM_UPSCALE_RATIO")) == "game", "Play holds Native (MetalFX AA) at the default with the spatial scaler")
        for s in aa { unsetenv(s.env); d.removeObject(forKey: "launcher." + s.env) }
        failed += checkLauncherLayout(LauncherModel(mac.settings, defaults: d), defaults: d)
        print("launcher storage: \(failed) failed")
        exit(failed == 0 ? 0 : 1)
    }
}
