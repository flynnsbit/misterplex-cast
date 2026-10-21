#!/usr/bin/env python3
"""Static libav loader/capability guard; no network or hardware."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import unittest
from unittest import mock
import uuid

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "arm_ffmpeg_static", ROOT / "scripts/check_arm_ffmpeg_static.py")
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class StaticLibav(unittest.TestCase):
    def setUp(self):
        self.prefix = ROOT / "build" / ("static-libav-policy-" + uuid.uuid4().hex)
        (self.prefix / "lib").mkdir(parents=True)
        self.addCleanup(shutil.rmtree, self.prefix)
        blob = (
            b"!<arch>\n--enable-decoder='" + guard.EXPECTED_DECODERS.encode() +
            b"' --enable-protocol='file,http,tcp,pipe,https,tls,udp' --enable-openssl "
            b"FFmpeg version " + guard.FFMPEG_VERSION.encode() + b"\n")
        for archive in guard.ARCHIVES:
            (self.prefix / "lib" / archive).write_bytes(blob)
        self.symbols = "".join("00000000 T " + name + "\n" for name in guard.REQUIRED)

    def check(self, extra="", code=0):
        result = subprocess.CompletedProcess([], code, self.symbols + extra, "")
        with mock.patch.object(guard.subprocess, "run", return_value=result) as run:
            guard.check_archives(self.prefix, "fixture-arm-nm")
        self.assertEqual(run.call_args.args[0][:2], ["fixture-arm-nm", "-g"])

    def test_safe_complete_archives(self):
        self.check("         U av_malloc\n")

    def test_iconv_and_other_module_loaders_are_refused(self):
        for symbol in guard.MODULE_LOADERS:
            with self.subTest(symbol=symbol):
                with self.assertRaisesRegex(guard.Refused, "runtime module loader"):
                    self.check("         U " + symbol + "\n")

    def test_versioned_and_defined_iconv_symbols_are_refused(self):
        for text in ("         U iconv_open@GLIBC_2.4\n", "00000000 T iconv\n"):
            with self.subTest(symbol=text), self.assertRaises(guard.Refused):
                self.check(text)

    def test_missing_required_features_are_refused(self):
        for symbol in guard.REQUIRED:
            with self.subTest(symbol=symbol):
                old = self.symbols
                self.symbols = self.symbols.replace("00000000 T " + symbol + "\n", "")
                with self.assertRaisesRegex(guard.Refused, "required HTTP"):
                    self.check("         U " + symbol + "\n")
                self.symbols = old

    def test_missing_archive_is_refused(self):
        (self.prefix / "lib" / guard.ARCHIVES[0]).unlink()
        with self.assertRaisesRegex(guard.Refused, "all four"):
            self.check()

    def test_undefined_weak_protocol_is_not_a_capability(self):
        self.symbols = self.symbols.replace("00000000 T ff_http_protocol\n", "")
        for kind in ("U", "w", "v"):
            with self.subTest(kind=kind), self.assertRaisesRegex(guard.Refused, "required HTTP"):
                self.check("         " + kind + " ff_http_protocol\n")

    def test_unreadable_symbols_are_refused(self):
        with self.assertRaisesRegex(guard.Refused, "cannot inspect"):
            self.check(code=1)

    def test_decoder_list_must_match_the_configure_line(self):
        util = self.prefix / "lib" / "libavutil.a"
        util.write_bytes(b"!<arch>\n--enable-decoder='h264,ass' FFmpeg version 8.1.2\n")
        with self.assertRaisesRegex(guard.Refused, "enable-decoder"):
            self.check()

    def test_missing_configure_string_is_refused(self):
        (self.prefix / "lib" / "libavutil.a").write_bytes(b"!<arch>\n")
        with self.assertRaisesRegex(guard.Refused, "no embedded"):
            self.check()

    def test_wrong_ffmpeg_version_is_refused(self):
        util = self.prefix / "lib" / "libavutil.a"
        util.write_bytes(
            b"!<arch>\n--enable-decoder='" + guard.EXPECTED_DECODERS.encode() +
            b"' FFmpeg version 4.4.4\n")
        with self.assertRaisesRegex(guard.Refused, "8.1.2"):
            self.check()

    def test_subtitle_decoder_symbols_are_refused(self):
        for symbol in ("ff_ass_decoder", "ff_srt_decoder", "ff_dvdsub_decoder",
                       "ff_webvtt_decoder", "ff_libzvbi_teletext_decoder"):
            with self.subTest(symbol=symbol):
                with self.assertRaisesRegex(guard.Refused, "subtitle decoder"):
                    self.check("00000000 T " + symbol + "\n")
                with self.assertRaisesRegex(guard.Refused, "subtitle decoder"):
                    self.check("         U " + symbol + "\n")

    def test_configure_hash_names_the_prefix(self):
        digest = guard.config_hash()
        self.assertEqual(len(digest), 12)
        self.assertTrue(all(c in "0123456789abcdef" for c in digest))
        self.assertEqual(guard.prefix_dirname(), "ffmpeg-arm-" + digest + "-time64")
        self.assertIn("--enable-decoder=" + guard.EXPECTED_DECODERS, guard.CONFIGURE_FLAGS)
        self.assertIn("--enable-protocol=file,http,tcp,pipe,https,tls,udp", guard.CONFIGURE_FLAGS)
        self.assertIn("--enable-openssl", guard.CONFIGURE_FLAGS)
        self.assertIn("ff_https_protocol", guard.REQUIRED)
        self.assertIn("ff_tls_protocol", guard.REQUIRED)
        for name in ("ass", "srt", "subrip", "webvtt", "dvdsub", "movtext"):
            self.assertNotIn(name, guard.EXPECTED_DECODERS.split(","))

    def make_plan(self, target):
            output = self.prefix / "candidate" / "misterplexd.debug"
            result = subprocess.run(
                ["make", "-s", "-n", target, "ARM_CXX=fixture-arm-g++",
                 "ARM_PLEXD_OUTPUT=" + str(output)], cwd=ROOT,
                text=True, capture_output=True, check=True)
            return output, result.stdout

    def test_daemon_only_candidate_does_not_build_auxiliaries(self):
            output, plan = self.make_plan("arm-plexd-daemon")
            self.assertIn('-o "' + str(output) + '"', plan)
            self.assertIn("check_arm_ffmpeg_static.py", plan)
            self.assertIn("ffmpeg-arm-" + guard.config_hash() + "-time64", plan)
            for name in ("ddr_write_bench", "push_frame", "set_status", "input_mailbox_probe"):
                self.assertNotIn(str(ROOT / "build/arm" / name), plan)
            self.assertFalse(output.exists(), "make dry-run wrote a candidate")

    def test_arm_ffmpeg_passes_the_hashed_build_dir(self):
            result = subprocess.run(
                ["make", "-s", "-n", "arm-ffmpeg"], cwd=ROOT, text=True, capture_output=True, check=True)
            plan = result.stdout
            digest = guard.config_hash()
            self.assertIn('FFMPEG_BUILD_DIR="', plan)
            self.assertIn("ffmpeg-8.1.2-" + digest + "-time64-build", plan)
            self.assertIn("ffmpeg-arm-" + digest + "-time64", plan)
            self.assertNotIn("ffmpeg-arm-static-safe", plan)

    def test_protocol_list_without_https_is_refused(self):
        util = self.prefix / "lib" / "libavutil.a"
        util.write_bytes(
            b"!<arch>\n--enable-decoder='" + guard.EXPECTED_DECODERS.encode() +
            b"' --enable-protocol='file,http,tcp,pipe' --enable-openssl "
            b"FFmpeg version " + guard.FFMPEG_VERSION.encode() + b"\n")
        with self.assertRaisesRegex(guard.Refused, "https"):
            self.check()

    def test_openssl_disabled_is_refused(self):
        util = self.prefix / "lib" / "libavutil.a"
        util.write_bytes(
            b"!<arch>\n--enable-decoder='" + guard.EXPECTED_DECODERS.encode() +
            b"' --enable-protocol='file,http,tcp,pipe,https,tls,udp' "
            b"FFmpeg version " + guard.FFMPEG_VERSION.encode() + b"\n")
        with self.assertRaisesRegex(guard.Refused, "OpenSSL"):
            self.check()

    def test_time64_stamp_is_required_and_a_32_bit_stamp_is_refused(self):
        with self.assertRaisesRegex(guard.Refused, "TIME_T_BITS"):
            guard.check_time64(self.prefix)
        (self.prefix / "TIME_T_BITS").write_text("32\n")
        with self.assertRaisesRegex(guard.Refused, "not 64"):
            guard.check_time64(self.prefix)
        (self.prefix / "TIME_T_BITS").write_text("64\n")
        guard.check_time64(self.prefix)

    def test_build_script_uses_one_time64_toolchain(self):
        text = (ROOT / "scripts/build_arm_ffmpeg.sh").read_text()
        linux = text.find("arm-linux-gnueabihf-gcc")
        none = text.find("arm-none-linux-gnueabihf-gcc")
        self.assertGreater(linux, 0)
        self.assertGreater(none, linux)
        self.assertIn("-D_TIME_BITS=64", text)
        self.assertIn("TIME_T_BITS", text)
        self.assertLess(text.find("arm-linux-gnueabihf-gcc"), text.find("arm-none-linux-gnueabihf-gcc"))

    def test_default_arm_build_retains_all_auxiliaries(self):
            output, plan = self.make_plan("arm-plexd")
            self.assertIn('-o "' + str(output) + '"', plan)
            for name in ("ddr_write_bench", "push_frame", "set_status", "input_mailbox_probe"):
                self.assertIn(str(ROOT / "build/arm" / name), plan)


if __name__ == "__main__":
    unittest.main()
