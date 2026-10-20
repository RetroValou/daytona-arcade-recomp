"""Host-side tests for Vita cross-build validation; no SDK or ROM required."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("build_vita", Path(__file__).resolve().parents[2] / "scripts/build_vita.py")
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class BuildVitaTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / "sdk with spaces"
        (self.sdk / "share").mkdir(parents=True)
        (self.sdk / "share/vita.toolchain.cmake").touch()
        patcher = patch.object(BUILD, "ROOT", self.root)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.output = io.StringIO()

    def call(self, *args):
        with contextlib.redirect_stdout(self.output), contextlib.redirect_stderr(self.output):
            return BUILD.main(["--vitasdk", str(self.sdk), *args])

    def test_missing_sdk(self):
        with self.assertRaises(SystemExit):
            self.call("--vitasdk", str(self.root / "missing"))
        self.assertIn("missing VitaSDK toolchain", self.output.getvalue())

    def test_missing_generated_code(self):
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call()
            run.assert_not_called()
        self.assertIn("host-generated code", self.output.getvalue())

    def test_reject_host_directory(self):
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--build-dir", "build_daytona/pc/daytona93")

    def test_reject_generated_directory(self):
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--build-dir", "build_daytona/recomp/daytona93/gen/vita")

    def test_reject_nonpositive_jobs(self):
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--jobs", "0")

    def test_compile_check_skips_rom_and_never_executes_importers(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.assertEqual(self.call("--compile-check"), 0)
        self.assertEqual(run.call_count, 2)
        configure, compile_ = run.call_args_list
        self.assertEqual(configure.args[0][0], "cmake")
        self.assertIn("-DDAYTONA_VITA_COMPILE_CHECK=ON", configure.args[0])
        self.assertIn(f"-DCMAKE_TOOLCHAIN_FILE={self.sdk}/share/vita.toolchain.cmake", configure.args[0])
        self.assertEqual(compile_.args[0][0:2], ["cmake", "--build"])
        self.assertEqual(configure.kwargs["env"]["VITASDK"], str(self.sdk))
        self.assertTrue(configure.kwargs["env"]["PATH"].startswith(str(self.sdk / "bin") + os.pathsep))
        self.assertIn("No linked game or VPK", self.output.getvalue())

    def test_default_optimization_enabled(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check")
        self.assertIn("-DDAYTONA_VITA_RENDER_OPT=ON", run.call_args_list[0].args[0])

    def test_reference_renderer(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--reference-renderer")
        self.assertIn("-DDAYTONA_VITA_RENDER_OPT=OFF", run.call_args_list[0].args[0])

    def test_success_requires_package(self):
        for name in BUILD.REQUIRED:
            path = self.root / "build_daytona/recomp/daytona93/gen" / name.format(set="daytona93")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()  # presence marker, not generated game code
        with patch.object(BUILD.subprocess, "run"):
            with self.assertRaises(RuntimeError):
                self.call()

    def test_gpu_package_rejects_missing_hook(self):
        (self.root / "seeds").mkdir()
        (self.root / "seeds/daytona93_hooks.txt").write_text("# a comment\n00017078 draw_list\n")
        for name in BUILD.REQUIRED:
            path = self.root / "build_daytona/recomp/daytona93/gen" / name.format(set="daytona93")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call()
            run.assert_not_called()
        self.assertIn("lacks the hooks draw_list", self.output.getvalue())

    def test_cores_pinned_by_default(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check")
        self.assertIn("-DDAYTONA_VITA_FREE_CORES=OFF", run.call_args_list[0].args[0])

    def test_free_core(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--free-core")
        configure = run.call_args_list[0].args[0]
        self.assertIn("-DDAYTONA_VITA_FREE_CORES=ON", configure)
        self.assertFalse(any("GPU_GL" in str(a) or "GPU_FAST" in str(a) for a in configure))

    def test_link_play_follows_the_game(self):
        # Only Daytona's link play was tested on Vitas: in for it, out for the others.
        for game, expected in (("daytona", "ON"), ("segarally", "OFF"), ("virtuafighter2", "OFF")):
            with patch.object(BUILD.subprocess, "run") as run:
                self.call("--compile-check", "--game", game)
            self.assertIn(f"-DDAYTONA_VITA_LINK={expected}", run.call_args_list[0].args[0])

    def test_link_play_override(self):
        for flag, game, expected in (("--link", "virtuafighter2", "ON"), ("--no-link", "daytona", "OFF")):
            with patch.object(BUILD.subprocess, "run") as run:
                self.call("--compile-check", "--game", game, flag)
            self.assertIn(f"-DDAYTONA_VITA_LINK={expected}", run.call_args_list[0].args[0])
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--link", "--no-link")

    def test_renderer_flags_are_gone(self):
        # vitaGL is the only renderer: --gpu-gl and --gpu-fast no longer exist.
        for flag in ("--gpu-gl", "--gpu-fast"):
            with self.assertRaises(SystemExit):
                self.call("--compile-check", flag)

    def test_custom_cross_directory(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--build-dir", "cross build")
        configure = run.call_args_list[0].args[0]
        self.assertIn(f"{self.root}/cross build", map(str, configure))
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/build_daytona/recomp/daytona93/gen", configure)

    def test_livearea_summary_is_per_game(self):
        self.assertEqual(BUILD.livearea_summary("daytona"), "none (Vita defaults)")
        folder = self.root / "platform/vita/logo_livearea/segarally"
        folder.mkdir(parents=True)
        (folder / ".gitkeep").touch()
        (folder / "bg.png").touch()
        (folder / "icon0.png").touch()
        self.assertEqual(BUILD.livearea_summary("segarally"), "icon0.png, bg.png")
        self.assertEqual(BUILD.livearea_summary("daytona"), "none (Vita defaults)")
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--game", "segarally")
        self.assertIn(f"-DDAYTONA_VITA_LIVEAREA_DIR={folder}", run.call_args_list[0].args[0])

    def test_title_id_follows_the_game(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check")
        configure = run.call_args_list[0].args[0]
        self.assertIn("-DDAYTONA_VITA_TITLEID=DAYT00093", configure)  # always passed: never a stale cache
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/build_daytona/recomp/daytona93/gen", configure)
        self.assertIn(f"{self.root}/build_daytona/vita", map(str, configure))
        self.assertIn("-DDAYTONA_VITA_VPK=daytona.vpk", configure)
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--game", "segarally")
        configure = run.call_args_list[0].args[0]
        self.assertIn("-DDAYTONA_VITA_ROMSET=srallyc", configure)
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/build_segarally/recomp/srallyc/gen", configure)
        self.assertIn(f"{self.root}/build_segarally/vita", map(str, configure))
        self.assertIn("-DDAYTONA_VITA_VPK=segarally.vpk", configure)
        self.assertIn("-DDAYTONA_VITA_TITLEID=SRAL00094", configure)
        self.assertIn("-DDAYTONA_VITA_GAME_NAME=Sega Rally Championship", configure)
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--set", "srallyc")  # the set alone gives its game
        self.assertIn("-DDAYTONA_VITA_VPK=segarally.vpk", run.call_args_list[0].args[0])

    def test_diagnostics_package_has_its_own_folder_and_name(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--game", "segarally", "--diagnostics")
        configure = run.call_args_list[0].args[0]
        self.assertIn(f"{self.root}/build_segarally/vita_diag", map(str, configure))
        self.assertIn("-DDAYTONA_VITA_VPK=segarally_diag.vpk", configure)
        self.assertIn("-DDAYTONA_VITA_TITLEID=SRAL90094", configure)

    def test_revision_a_uses_the_games_folder(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--set", "daytona")
        configure = run.call_args_list[0].args[0]
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/build_daytona/recomp/daytona/gen", configure)
        self.assertIn("-DDAYTONA_VITA_VPK=daytona.vpk", configure)

    def test_set_must_be_the_games(self):
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call("--compile-check", "--game", "segarally", "--set", "daytona93")
            run.assert_not_called()
        self.assertIn("not a set of segarally", self.output.getvalue())

    def test_title_id_override_is_checked(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--game", "segarally", "--title-id", "SRAL00001")
        self.assertIn("-DDAYTONA_VITA_TITLEID=SRAL00001", run.call_args_list[0].args[0])
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--title-id", "sral1")

    def test_all_rejects_one_games_options(self):
        for option in (["--set", "srallyc"], ["--title-id", "ABCD12345"], ["--build-dir", "x"]):
            with self.assertRaises(SystemExit):
                self.call("--game", "all", *option)
            self.assertIn("not with --game all", self.output.getvalue())

    def test_all_builds_the_games_with_roms_and_the_same_options(self):
        present = {"srallyc", "vf2", "fvipers", "manxttc"}  # fvipers: no Vita package yet
        calls = []
        def fake_main(argv):
            calls.append(argv)
            return 1 if "manxtt" in argv else 0
        for name in BUILD.REQUIRED:  # host code present: no recompilation
            for romset in present:
                path = self.root / BUILD.games.gen_root(romset) / name.format(set=romset)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
        with patch.object(BUILD.games, "rom_archives", lambda romset: ["x"] if romset in present else []), \
             patch.object(BUILD, "main", fake_main), patch.object(BUILD.subprocess, "run") as run, \
             contextlib.redirect_stdout(self.output):
            status = BUILD.build_all(["--game", "all", "--release", "--fast-inaccuracy"], "fast_inaccuracy")
        run.assert_not_called()
        self.assertEqual(status, 1)  # manxtt failed; the others went on
        built = [argv[argv.index("--game") + 1] for argv in calls]
        self.assertEqual(built, ["segarally", "virtuafighter2", "manxtt"])
        self.assertTrue(all(argv[:2] == ["--release", "--fast-inaccuracy"] for argv in calls))
        text = self.output.getvalue()
        self.assertIn("skipped: no roms/daytona93", text)
        self.assertIn("fightingvipers   skipped: its Vita package does not work yet", text)
        self.assertIn("manxtt           FAILED", text)

    def test_all_skips_a_mode_not_checked(self):
        with patch.object(BUILD.games, "rom_archives", lambda romset: ["x"] if romset == "srallyc" else []), \
             patch.object(BUILD, "main", lambda argv: 0), contextlib.redirect_stdout(self.output):
            self.assertEqual(BUILD.build_all(["--game", "all", "--fast-gen"], "fast_gen"), 0)
        self.assertIn("skipped: --fast-gen not checked on srallyc", self.output.getvalue())

    def test_srallyc_has_no_fast_gen(self):
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call("--set", "srallyc", "--fast-gen")
            run.assert_not_called()

    def test_srallyc_fast_inaccuracy_is_accepted(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--set", "srallyc", "--fast-inaccuracy")
        run.assert_called()

    def test_inline_bus_is_opt_in(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--fast-inaccuracy")
        self.assertIn("-DDAYTONA_VITA_INLINE_BUS=OFF", run.call_args_list[0].args[0])
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--fast-inaccuracy", "--inline-bus")
        self.assertIn("-DDAYTONA_VITA_INLINE_BUS=ON", run.call_args_list[0].args[0])


FAST_GEN_SPEC = importlib.util.spec_from_file_location(
    "fast_gen", Path(__file__).resolve().parents[2] / "scripts/fast_gen.py")
FAST_GEN = importlib.util.module_from_spec(FAST_GEN_SPEC)
FAST_GEN_SPEC.loader.exec_module(FAST_GEN)


class FastGenInlineBusTests(unittest.TestCase):
    """fast_gen.py on m2recomp --board code (Mem::, the bus inlined)."""

    def test_fixed_work_ram_accesses_become_direct(self):
        stats = {"wram": 0}
        line = "R[3] = Mem::ld32(B, 0x00512340u); Mem::st16(B, 0x00500002u, uint16_t(R[4]));"
        self.assertEqual(FAST_GEN.direct_wram(line, stats),
                         "R[3] = gen::wram_r32(c, 0x12340u); gen::wram_w16(c, 0x00002u, uint16_t(R[4]));")
        self.assertEqual(stats["wram"], 2)

    def test_wait_loop_addresses_and_unaligned_keep_the_bus(self):
        stats = {"wram": 0}
        line = "R[3] = Mem::ld8(B, 0x00500000u); R[4] = Mem::ld32(B, 0x00500002u); R[5] = Mem::ld32(B, 0x0054b150u);"
        self.assertEqual(FAST_GEN.direct_wram(line, stats, {0x0, 0x4b150}), line)
        self.assertEqual(stats["wram"], 0)

    def test_a_byte_no_wait_loop_reads_becomes_direct(self):
        stats = {"wram": 0}
        self.assertEqual(FAST_GEN.direct_wram("R[3] = Mem::ld8(B, 0x00500000u);", stats), "R[3] = gen::wram_r8(c, 0x00000u);")
        self.assertEqual(stats["wram"], 1)

    def test_inlined_bus_access_is_not_register_only(self):
        self.assertFalse(FAST_GEN.pure(["    R[3] = Mem::ld32(B, R[4]);"]))
        self.assertTrue(FAST_GEN.pure(["    R[3] = gen::wram_r32(c, 0x00010u);"]))


class WaitLoopAddressTests(unittest.TestCase):
    """scripts/games.py "wram_bus" against the games' records (src/games/*.cpp
    Game::spin): every wait loop's byte or word in work RAM, and nothing else."""

    def test_wram_bus_matches_the_records(self):
        import re
        root = Path(__file__).resolve().parents[2]
        spins = {}
        for f in sorted((root / "src/games").glob("*.cpp")):
            for record in f.read_text().split("extern const Game ")[1:]:
                name = re.search(r'\.set = "(\w+)"', record)
                if not name:  # games.cpp's declarations
                    continue
                name = name.group(1)
                spins[name] = {int(a, 16) for a in re.findall(r"SpinWait::\w+, 0x[0-9a-f]+, (0x[0-9a-f]+)", record)
                               if 0x00500000 <= int(a, 16) < 0x00600000}
        for name, entry in BUILD.SETS.items():
            self.assertEqual(set(entry["wram_bus"]), spins[name], name)


if __name__ == "__main__":
    unittest.main()
