#!/usr/bin/env python3
"""Compile and run host tests against production firmware algorithms."""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True

from validation_support import ROOT, ValidationError, host_cc, run


def compile_and_run(name: str, sources: list[Path], extra: list[str] | None = None,
                    *, sanitize: bool = False) -> str:
    compiler = host_cc()
    with tempfile.TemporaryDirectory(prefix=f"alt2-{name}-") as temporary:
        binary = Path(temporary) / name
        command = [
            compiler,
            "-std=c11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            *(["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
              if sanitize else []),
            "-I",
            ROOT / "Core" / "Inc",
            f'-DHELP_BIN_PATH="{ROOT / "sd" / "HELP.BIN"}"',
            f'-DCWPACK_BIN_PATH="{ROOT / "sd" / "CWPACK.BIN"}"',
            "-DHELP_SETTLE_MS=0U",  # the suites read each help text once
            *(extra or []),
            *sources,
            "-lm",
            "-o",
            binary,
        ]
        run(command)
        result = run([binary])
        return result.stdout.strip()


def execute(*, sanitize: bool = False,
            selected: set[str] | None = None) -> list[dict[str, str]]:
    tests = ROOT / "tools" / "tests"
    source = ROOT / "Core" / "Src"
    specifications = [
        (
            "fw_update_format",
            [tests / "fw_update_format_test.c"],
            [],
        ),
        (
            "transport_logic",
            [tests / "transport_logic_test.c", source / "mod_transport_logic.c"],
            [],
        ),
        (
            "modulation_reference",
            [tests / "modulation_reference_test.c", source / "mod_transport_logic.c"],
            [],
        ),
        (
            "mode_math",
            [tests / "mode_math_test.c"],
            [],
        ),
        (
            "audio_control_sanitize",
            [tests / "audio_control_sanitize_test.c"],
            [],
        ),
        (
            "mod_api_audio_sanitize",
            [tests / "mod_api_audio_sanitize_test.c", source / "mod_api_audio.c"],
            [
                "-DSTM32G474xx",
                "-DUSE_HAL_DRIVER",
                "-DMOD_HOST_TEST_CRITICAL_STUBS=1",
                "-Wno-int-to-pointer-cast",
                "-ffunction-sections",
                "-fdata-sections",
                "-Wl,--gc-sections",
                "-I",
                ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Include",
            ],
        ),
        (
            "analytic_reference",
            [tests / "analytic_reference_test.c"],
            [],
        ),
        (
            "analytic_policy_normal",
            [tests / "analytic_build_policy_test.c"],
            [],
        ),
        (
            "analytic_policy_disabled",
            [tests / "analytic_build_policy_test.c"],
            ["-DMOD_SSB_ANALYTIC_EXPERIMENT_ENABLE=0"],
        ),
        (
            "analytic_compile_policy_disabled",
            [tests / "analytic_compile_policy_test.c"],
            ["-DMOD_SSB_ANALYTIC_EXPERIMENT_ENABLE=0"],
        ),
        (
            "analytic_policy_diagnostic",
            [tests / "analytic_build_policy_test.c"],
            ["-DMOD_DEEP_DIAG_ENABLE=1"],
        ),
        (
            "ui_menu_policy_normal",
            [tests / "ui_menu_policy_test.c", source / "ui_menu.c", source / "ui_help.c",
             tests / "stubs" / "sd_help_card.c"],
            [
                "-DSTM32G474xx",
                "-DUSE_HAL_DRIVER",
                "-Wno-int-to-pointer-cast",
                "-ffunction-sections",
                "-fdata-sections",
                "-Wl,--gc-sections",
                "-I",
                ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Include",
            ],
        ),
        (
            "ui_menu_policy_diagnostic",
            [tests / "ui_menu_policy_test.c", source / "ui_menu.c", source / "ui_help.c",
             tests / "stubs" / "sd_help_card.c"],
            [
                "-DMOD_DEEP_DIAG_ENABLE=1",
                "-ffunction-sections",
                "-fdata-sections",
                "-Wl,--gc-sections",
                "-DSTM32G474xx",
                "-DUSE_HAL_DRIVER",
                "-Wno-int-to-pointer-cast",
                "-I",
                ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Include",
            ],
        ),
        (
            "ui_format",
            [tests / "ui_format_test.c", source / "ui_format.c"],
            [],
        ),
        (
            "ui_menu_model_normal",
            [tests / "ui_menu_model_test.c", source / "ui_menu.c", source / "ui_settings_map.c", source / "ui_help.c", tests / "stubs" / "sd_help_card.c"],
            [
                "-DUI_SETTINGS_POLICY_ONLY=1",
                "-DSTM32G474xx",
                "-DUSE_HAL_DRIVER",
                "-Wno-int-to-pointer-cast",
                "-ffunction-sections",
                "-fdata-sections",
                "-Wl,--gc-sections",
                "-I",
                ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Include",
            ],
        ),
        (
            "ui_menu_model_diagnostic",
            [tests / "ui_menu_model_test.c", source / "ui_menu.c", source / "ui_settings_map.c", source / "ui_help.c", tests / "stubs" / "sd_help_card.c"],
            [
                "-DUI_SETTINGS_POLICY_ONLY=1",
                "-DMOD_DEEP_DIAG_ENABLE=1",
                "-DSTM32G474xx",
                "-DUSE_HAL_DRIVER",
                "-Wno-int-to-pointer-cast",
                "-ffunction-sections",
                "-fdata-sections",
                "-Wl,--gc-sections",
                "-I",
                ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Include",
            ],
        ),
        (
            "ui_gfx",
            [tests / "ui_gfx_test.c", source / "ui_gfx.c"],
            [],
        ),
        (
            "cat_logic",
            [tests / "cat_logic_test.c", source / "cat_logic.c"],
            [],
        ),
        (
            "slot_logic",
            [tests / "slot_logic_test.c", source / "radio_slot_logic.c"],
            [],
        ),
        (
            "ui_tune",
            [tests / "ui_tune_test.c", source / "ui_tune.c"],
            [
                "-DSTM32G474xx",
                "-DUSE_HAL_DRIVER",
                "-Wno-int-to-pointer-cast",
                "-I",
                ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Include",
            ],
        ),
        (
            "ui_settings_policy_normal",
            [tests / "ui_settings_policy_test.c", source / "ui_settings_map.c", source / "ui_help.c", tests / "stubs" / "sd_help_card.c"],
            [
                "-DUI_SETTINGS_POLICY_ONLY=1",
                "-DSTM32G474xx",
                "-DUSE_HAL_DRIVER",
                "-Wno-int-to-pointer-cast",
                "-ffunction-sections",
                "-fdata-sections",
                "-Wl,--gc-sections",
                "-I",
                ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Include",
            ],
        ),
        (
            "ui_settings_policy_disabled",
            [tests / "ui_settings_policy_test.c", source / "ui_settings_map.c", source / "ui_help.c", tests / "stubs" / "sd_help_card.c"],
            [
                "-DUI_SETTINGS_POLICY_ONLY=1",
                "-DMOD_SSB_ANALYTIC_EXPERIMENT_ENABLE=0",
                "-DSTM32G474xx",
                "-DUSE_HAL_DRIVER",
                "-Wno-int-to-pointer-cast",
                "-ffunction-sections",
                "-fdata-sections",
                "-Wl,--gc-sections",
                "-I",
                ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Include",
            ],
        ),
        (
            "ui_settings_policy_diagnostic",
            [tests / "ui_settings_policy_test.c", source / "ui_settings_map.c", source / "ui_help.c", tests / "stubs" / "sd_help_card.c"],
            [
                "-DUI_SETTINGS_POLICY_ONLY=1",
                "-DMOD_DEEP_DIAG_ENABLE=1",
                "-DSTM32G474xx",
                "-DUSE_HAL_DRIVER",
                "-Wno-int-to-pointer-cast",
                "-ffunction-sections",
                "-fdata-sections",
                "-Wl,--gc-sections",
                "-I",
                ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Include",
            ],
        ),
        (
            "calibration_logic",
            [tests / "calibration_logic_test.c"],
            [],
        ),
        (
            "alc_logic",
            [tests / "alc_logic_test.c"],
            [],
        ),
        (
            "cessb_policy",
            [tests / "cessb_policy_test.c"],
            [],
        ),
        (
            "ssb_envelope_policy",
            [tests / "ssb_envelope_policy_test.c"],
            [],
        ),
        (
            "ssb_gate_logic",
            [tests / "ssb_gate_logic_test.c"],
            [],
        ),
        (
            "digi_tone_logic",
            [tests / "digi_tone_logic_test.c"],
            [],
        ),
        (
            "radio_guard_logic",
            [tests / "radio_guard_logic_test.c"],
            [],
        ),
        (
            "sctrl_link_logic",
            [tests / "sctrl_link_logic_test.c", source / "sctrl_link_logic.c"],
            [],
        ),
        (
            "tx_bw_filter_state",
            [tests / "tx_bw_filter_state_test.c"],
            [
                "-DSTM32G474xx",
                "-DUSE_HAL_DRIVER",
                "-Wno-int-to-pointer-cast",
                "-I",
                ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Include",
            ],
        ),
        (
            "dual_envelope_logic",
            [tests / "dual_envelope_logic_test.c"],
            ["-DMOD_DUAL_ENVELOPE_EXPERIMENT_ENABLE=1"],
        ),
        (
            "sd_audio_math",
            [tests / "sd_audio_math_test.c", source / "sd_audio_math.c"],
            [],
        ),
        (
            "sd_driver",
            [tests / "sd_driver_test.c"],
            [],
        ),
        (
            # The import, backup/restore and calibration store against the
            # real FatFs on a RAM card (stubs/sd_fat_disk.c).
            "store_import",
            [
                tests / "store_import_test.c",
                tests / "stubs" / "sd_fat_disk.c",
                source / "mod_storage_import.c",
                source / "mod_sd_store.c",
                source / "mod_cal_storage.c",
                source / "mod_storage_format.c",
                source / "radio_config.c",
                source / "mod_crc32.c",
                ROOT / "Middlewares" / "Third_Party" / "FatFs" / "src" / "ff.c",
                ROOT / "Middlewares" / "Third_Party" / "FatFs" / "src" / "ffunicode.c",
            ],
            [
                "-fshort-enums",
                "-DSTM32G474xx",
                "-DUSE_HAL_DRIVER",
                "-Wno-int-to-pointer-cast",
                "-I", str(tests),
                "-I",
                ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Include",
            ],
        ),
        (
            "sd_store",
            [tests / "sd_store_test.c", source / "mod_sd_store.c", tests / "stubs" / "sd_store_disk.c"],
            ["-I", str(tests)],
        ),
        (
            # The production keyer, sampler and reader between a scripted
            # paddle and the 48 kHz step, as on the radio.
            "cw_keyer",
            [tests / "cw_keyer_test.c", source / "mod_cw_keyer.c"],
            [],
        ),
        (
            "morse_table",
            [tests / "morse_table_test.c", source / "mod_morse_tables.c"],
            [],
        ),
        (
            # The trainer's pack reader against the tree's sd/CWPACK.BIN, on a
            # card the suite can damage, cut short or take away.
            "cw_pack",
            [tests / "cw_pack_test.c", source / "cw_pack.c", source / "mod_morse_tables.c",
             tests / "stubs" / "sd_cw_card.c"],
            ["-I", tests / "stubs"],
        ),
        (
            # The trainer's player and the shared Morse table: PARIS cadence
            # to a sample, ARRL Farnsworth spacing, a ramped stop, prosigns.
            "cw_player",
            [tests / "cw_player_test.c", source / "mod_cw_player.c", source / "mod_morse_tables.c"],
            ["-DCWP_HOST=1"],
        ),
        (
            # The trainer with its player, the Morse table and the pack reader
            # against the tree's sd/CWPACK.BIN, a scripted keyer and a main
            # loop the suite can hold up.
            "morse_trainer",
            [tests / "morse_trainer_test.c", source / "mod_morse_trainer.c", source / "mod_cw_player.c",
             source / "mod_morse_tables.c", source / "cw_pack.c", tests / "stubs" / "sd_cw_card.c"],
            ["-I", tests / "stubs", "-DCWP_HOST=1", "-DMTRAIN_HOST=1"],
        ),
        (
            "storage_migration",
            [
                tests / "storage_migration_test.c",
                source / "mod_storage_format.c",
                source / "radio_config.c",
                source / "mod_crc32.c",
            ],
            [
                "-fshort-enums",
                "-DSTM32G474xx",
                "-DUSE_HAL_DRIVER",
                "-Wno-int-to-pointer-cast",
                "-I",
                ROOT / "Drivers" / "STM32G4xx_HAL_Driver" / "Inc",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Device" / "ST" / "STM32G4xx" / "Include",
                "-I",
                ROOT / "Drivers" / "CMSIS" / "Include",
            ],
        ),
    ]
    specifications += [
        ("rev1_policy", [tests / "rev1_policy_test.c", source / "radio_config.c", source / "radio_pd_policy.c"], []),
        ("rev1_front_filter", [tests / "rev1_front_filter_test.c", source / "radio_config.c", source / "radio_dsp.c", source / "radio_rx_front.c", source / "audio_resample.c"], []),
        ("rev1_audio", [tests / "rev1_audio_test.c", source / "radio_config.c", source / "radio_dsp.c", source / "radio_rx_front.c", source / "audio_resample.c"], []),
        ("cw_receive", [tests / "cw_receive_test.c", source / "radio_dsp.c", source / "radio_config.c"],
         ["-D_DEFAULT_SOURCE"]),
        ("cw_decoder", [tests / "cw_decoder_test.c", source / "radio_cw_decoder.c", source / "mod_morse_tables.c",
                        source / "radio_dsp.c", source / "radio_config.c"], []),
        ("rev1_bpf_sweep", [tests / "rev1_bpf_sweep_test.c", source / "radio_bpf_fit.c", source / "radio_config.c"], []),
        ("rev1_af_sweep", [tests / "rev1_af_sweep_test.c", source / "radio_af_fit.c"], []),
        ("rev1_mic_monitor", [tests / "rev1_mic_monitor_test.c", source / "radio_config.c"], []),
        ("rev1_mic_front", [tests / "rev1_mic_front_test.c"], []),
        ("usb_diag_keys", [tests / "usb_diag_keys_test.c", source / "usb_diag_keys.c"], []),
        ("rev1_rxcal", [tests / "rev1_rxcal_test.c", source / "radio_iq_fit.c"], []),
        ("rev1_lpf", [tests / "rev1_lpf_test.c", source / "radio_config.c"], []),
        ("rev1_selftest", [tests / "rev1_selftest_test.c", source / "mod_selftest_logic.c", source / "radio_config.c"], []),
        ("rf_meter", [tests / "rf_meter_test.c", source / "radio_rf_meter.c", source / "radio_config.c"],
         ["-D_DEFAULT_SOURCE"]),
        ("clock_plan", [tests / "clock_plan_test.c", source / "clock_plan.c", source / "radio_rx_front.c",
                        source / "radio_dsp.c", source / "radio_config.c", source / "mod_transport_logic.c"], []),
        ("clock_ref_scale", [tests / "clock_ref_scale_test.c", source / "clock_plan.c", source / "radio_rx_front.c",
                             source / "radio_dsp.c", source / "radio_config.c"], ["-DCLOCK_HSE_NOMINAL_HZ=16000000UL"]),
        ("clock_ref_scale_32", [tests / "clock_ref_scale_test.c", source / "clock_plan.c", source / "radio_rx_front.c",
                                source / "radio_dsp.c", source / "radio_config.c"], ["-DCLOCK_HSE_NOMINAL_HZ=32000000UL"]),
        ("clock_ref_scale_40", [tests / "clock_ref_scale_test.c", source / "clock_plan.c", source / "radio_rx_front.c",
                                source / "radio_dsp.c", source / "radio_config.c"], ["-DCLOCK_HSE_NOMINAL_HZ=40000000UL"]),
    ]
    specifications.append(("rev1_receiver", [tests / "rev1_receiver_test.c", source / "radio_rx_front.c",
                                              source / "radio_config.c", source / "radio_clocks.c"], [
        "-DSTM32G474xx", "-DUSE_HAL_DRIVER", "-DMOD_HOST_TEST_CRITICAL_STUBS=1", "-Wno-int-to-pointer-cast",
        "-I", ROOT / "Drivers/STM32G4xx_HAL_Driver/Inc",
        "-I", ROOT / "Drivers/CMSIS/Device/ST/STM32G4xx/Include",
        "-I", ROOT / "Drivers/CMSIS/Include",
    ]))
    specifications.append(("rev1_gpio", [tests / "rev1_gpio_test.c", source / "board_rev1.c"], [
        "-DSTM32G474xx", "-DUSE_HAL_DRIVER", "-DMOD_HOST_TEST_CRITICAL_STUBS=1", "-Wno-int-to-pointer-cast",
        "-I", ROOT / "Drivers/STM32G4xx_HAL_Driver/Inc",
        "-I", ROOT / "Drivers/CMSIS/Device/ST/STM32G4xx/Include",
        "-I", ROOT / "Drivers/CMSIS/Include",
    ]))
    specifications.append(("rev1_pd", [tests / "rev1_pd_test.c", source / "radio_config.c", source / "radio_pd_policy.c"], [
        "-DSTM32G474xx", "-DUSE_HAL_DRIVER", "-DUSBPD_PORT_COUNT=1", "-D_SNK", "-DUSBPDCORE_LIB_PD3_CONFIG_1",
        "-Wno-int-to-pointer-cast", "-Wno-pointer-to-int-cast",
        "-I", ROOT / "Drivers/STM32G4xx_HAL_Driver/Inc",
        "-I", ROOT / "Drivers/CMSIS/Device/ST/STM32G4xx/Include", "-I", ROOT / "Drivers/CMSIS/Include",
        "-I", ROOT / "Middlewares/ST/STM32_USBPD_Library/Core/inc",
        "-I", ROOT / "Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/inc",
    ]))
    # The settings help: every packed text against Core/Inc/ui_help_text.def.
    specifications.append(("ui_help", [tests / "ui_help_test.c", source / "ui_help.c", tests / "stubs" / "sd_help_card.c"], [
        "-DSTM32G474xx", "-DUSE_HAL_DRIVER", "-Wno-int-to-pointer-cast",
        "-I", ROOT / "Drivers/STM32G4xx_HAL_Driver/Inc",
        "-I", ROOT / "Drivers/CMSIS/Device/ST/STM32G4xx/Include",
        "-I", ROOT / "Drivers/CMSIS/Include",
    ]))
    # The waterfall: its arithmetic, and the engine against a simulated receiver.
    specifications.append(("spectrum_dsp", [tests / "spectrum_dsp_test.c"], []))
    specifications.append(("spectrum_engine", [tests / "spectrum_engine_test.c", source / "radio_spectrum.c",
                                                source / "radio_config.c"], [
        "-DSTM32G474xx", "-DUSE_HAL_DRIVER", "-DMOD_HOST_TEST_CRITICAL_STUBS=1", "-Wno-int-to-pointer-cast",
        "-I", ROOT / "Drivers/STM32G4xx_HAL_Driver/Inc",
        "-I", ROOT / "Drivers/CMSIS/Device/ST/STM32G4xx/Include",
        "-I", ROOT / "Drivers/CMSIS/Include",
    ]))
    available = {name for name, _sources, _extra in specifications}
    unknown = (selected or set()) - available
    if unknown:
        raise ValidationError(f"unknown host suite(s): {', '.join(sorted(unknown))}")
    if selected is not None:
        specifications = [
            specification for specification in specifications
            if specification[0] in selected
        ]
    results: list[dict[str, str]] = []
    for name, sources, extra in specifications:
        output = compile_and_run(name, sources, extra, sanitize=sanitize)
        results.append({"name": name, "output": output})
    return results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="emit machine-readable results")
    parser.add_argument("--sanitize", action="store_true",
                        help="run AddressSanitizer and UndefinedBehaviorSanitizer")
    parser.add_argument(
        "--test",
        action="append",
        metavar="NAME",
        help="run only this named suite (repeatable)",
    )
    args = parser.parse_args()
    try:
        if args.sanitize:
            os.environ.setdefault("ASAN_OPTIONS", "detect_leaks=0")
        results = execute(
            sanitize=args.sanitize,
            selected=set(args.test) if args.test else None,
        )
    except ValidationError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc)}))
        else:
            print(f"FAIL  {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps({"ok": True, "tests": results}, indent=2))
    else:
        for result in results:
            print(f"PASS  {result['output']}")
        print(f"PASS  {len(results)} host suites executed production code")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
