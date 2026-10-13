#!/usr/bin/env python3
"""Regression tests for standalone migrate script."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MIGRATE_SCRIPT = ROOT / "data" / "scripts" / "migrate"


def run_migrate(*args: str) -> tuple[int, str, str]:
    cmd = [sys.executable, str(MIGRATE_SCRIPT)] + list(args)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode, proc.stdout, proc.stderr


def test_dry_run_and_apply():
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp_path = Path(tmpdir)
        config_file = tmp_path / "config.json"
        conf_file = tmp_path / "vinput.conf"

        # Mock legacy config (< 2.3.15)
        legacy_config = {
            "version": 1,
            "global": {
                "capture_device": "default",
                "default_language": "zh_CN",
            },
            "asr": {
                "active_provider": "sherpa-onnx",
                "vad": {
                    "threshold": 0.45,
                    "min_speech_duration": 0.15,
                    "min_silence_duration": 0.5,
                    "speech_pad_ms": 30,
                },
            },
            "scenes": {
                "definitions": [
                    {
                        "id": "code",
                        "candidate_count": 3,
                        "show_raw": True,
                    }
                ]
            },
            "llm": {
                "adaptors": [
                    {
                        "id": "cjk-trim",
                        "command": "python3",
                    }
                ]
            },
        }
        config_file.write_text(json.dumps(legacy_config, indent=2), encoding="utf-8")

        # Mock legacy conf
        legacy_conf = "\n".join(
            [
                "[Trigger]",
                "SceneMenuKey=Alt+Shift+V",
                "AsrMenuKey=Control+Shift+V",
                "HoldActivationDelay=200",
                "",
            ]
        )
        conf_file.write_text(legacy_conf, encoding="utf-8")

        # 1. Test Dry Run
        code, out, err = run_migrate(
            "--dry-run",
            "--json",
            "--config",
            str(config_file),
            "--conf",
            str(conf_file),
        )
        assert code == 0, f"dry-run failed: {err}"
        dry_report = json.loads(out)
        assert dry_report["needed"] is True
        assert dry_report["applied"] is False
        assert dry_report["dry_run"] is True
        assert len(dry_report["changes"]) >= 6

        # Verify files were NOT modified during dry-run
        unchanged_cfg = json.loads(config_file.read_text(encoding="utf-8"))
        assert "candidate_count" in unchanged_cfg["scenes"]["definitions"][0]

        # 2. Test Apply
        code, out, err = run_migrate(
            "--json",
            "--config",
            str(config_file),
            "--conf",
            str(conf_file),
        )
        assert code == 0, f"apply failed: {err}"
        apply_report = json.loads(out)
        assert apply_report["needed"] is True
        assert apply_report["applied"] is True
        assert apply_report["dry_run"] is False
        assert Path(apply_report["config_backup"]).is_file()
        assert Path(apply_report["conf_backup"]).is_file()

        # 3. Verify in-place modified content
        migrated_cfg = json.loads(config_file.read_text(encoding="utf-8"))
        sc_def = migrated_cfg["scenes"]["definitions"][0]
        assert "count" in sc_def and "candidate_count" not in sc_def
        assert sc_def["count"] == 3
        assert "raw_cand" in sc_def and "show_raw" not in sc_def
        assert sc_def["raw_prev"] is True
        assert "adapters" in migrated_cfg["llm"] and "adaptors" not in migrated_cfg["llm"]
        assert "default_language" not in migrated_cfg["global"]
        assert migrated_cfg["asr"]["vad"]["threshold"] == 0.35
        assert migrated_cfg["asr"]["vad"]["min_speech_duration"] == 0.10
        assert migrated_cfg["asr"]["vad"]["min_silence_duration"] == 1.2
        assert migrated_cfg["asr"]["vad"]["speech_pad_ms"] == 500
        assert migrated_cfg["vinput_version"] == "2.4.1"

        migrated_conf = conf_file.read_text(encoding="utf-8")
        assert "MenuKey=Alt+Shift+V" in migrated_conf
        assert "SceneMenuKey" not in migrated_conf
        assert "AsrMenuKey" not in migrated_conf
        assert "HoldActivationDelay" not in migrated_conf

        # 4. Test Idempotency (run again should detect needed == false)
        code, out, err = run_migrate(
            "--json",
            "--config",
            str(config_file),
            "--conf",
            str(conf_file),
        )
        assert code == 0, f"idempotent run failed: {err}"
        idempotent_report = json.loads(out)
        assert idempotent_report["needed"] is False
        assert idempotent_report["applied"] is False


if __name__ == "__main__":
    test_dry_run_and_apply()
    print("All migrate script tests passed!")
