#!/usr/bin/env python3
"""Consumer-level checks for test suite discovery and GNU Make routing."""

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = {
    "default": "test",
    "all": "test-all",
    "native": "test-native",
    "tooling": "test-tooling",
    "monad": "test-monad",
    "frame": "test-frame",
    "native-profile": "test-native-profile",
    "agent-api": "test-agent-api-compile",
    "agent-runtime": "test-agent-runtime",
    "keychords-monad": "test-keychords-monad",
    "keychords-performance": "test-keychords-performance",
    "renderer2d-capacity": "test-renderer2d-capacity",
    "emacs-reference": "test-emacs-reference",
    "emacs-scene-undo": "test-emacs-scene-undo",
    "sculpture-runtime": "test-sculpture-runtime",
    "semantic-preflight": "test-semantic-preflight",
    "catalog-transition": "test-catalog-transition",
    "export-catalog": "test-export-catalog",
    "native-read": "test-native-read",
    "headless-startup-render": "test-headless-startup-render",
    "headless-sponza": "test-headless-sponza",
    "vulkan-validation": "test-vulkan-validation-headless",
    "middle-orbit": "test-headless-middle-orbit",
    "editor-keymaps": "test-editor-keymaps",
    "headless-editor-keymaps": "test-headless-editor-keymaps",
    "repl-abi": "test-repl-abi",
    "font-unicode": "test-font-unicode",
    "scene-gesture": "test-scene-gesture",
    "sdf-viewport": "test-sdf-viewport",
    "sdf-typed-viewport": "test-sdf-typed-viewport",
    "sdf-viewport-runtime": "test-sdf-viewport-runtime",
    "viewport-coordinates": "test-viewport-coordinates",
    "sdf-property-bridge": "test-sdf-property-bridge",
    "sdf-property-gesture": "test-sdf-property-gesture",
    "sdf-appearance-clipboard": "test-sdf-appearance-clipboard",
}
LEGACY_TARGETS = (
    "all", "libs", "debug", "run", "install", "uninstall",
    "clean", "distclean", "rebuild", "help",
)


def invoke(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["./make", *args], cwd=ROOT, text=True, capture_output=True, check=False,
    )


def main() -> None:
    listed = invoke("test", "--list", "--color", "never")
    assert listed.returncode == 0, listed.stderr
    rows = {}
    for line in listed.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] in EXPECTED:
            rows[fields[0]] = fields[1]
    assert rows == EXPECTED, (rows, listed.stdout)
    assert "host: native, tooling" in listed.stdout
    assert "integration: monad, frame" in listed.stdout

    help_topic = invoke("help", "test", "--color", "never")
    assert help_topic.returncode == 0 and rows.keys() <= set(help_topic.stdout.split()), (
        help_topic.returncode, help_topic.stdout, help_topic.stderr,
    )

    bad_suite = invoke("test", "not-a-suite", "--color", "never")
    assert bad_suite.returncode == 2 and "Unknown test suite" in bad_suite.stdout
    wrong_option = invoke("test", "--runs", "1", "--color", "never")
    assert wrong_option.returncode == 2 and "do not apply to 'test'" in wrong_option.stdout

    # Dry-run the existing Make targets behind every catalog row. This checks
    # the wrapper contract without executing GPU, SDK, or destructive recipes.
    targets = sorted(set(EXPECTED.values()) | set(LEGACY_TARGETS))
    backend = subprocess.run(
        ["make", "-n", *targets], cwd=ROOT, text=True, capture_output=True, check=False,
    )
    assert backend.returncode == 0, backend.stderr
    print(f"test CLI catalog and existing Make target routes PASS ({len(rows)} suites)")


if __name__ == "__main__":
    main()
