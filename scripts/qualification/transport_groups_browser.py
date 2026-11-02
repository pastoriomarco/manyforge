"""Small real-browser group-selection check; fixture only, no robot backend."""
from __future__ import annotations

import argparse
import time

from current_workbench_browser_collector import RawWebDriver, _execute


def run(endpoint: str, base_url: str) -> None:
    driver = RawWebDriver(endpoint, timeout_s=15)
    try:
        driver.create_session(firefox_args=("-headless", "-no-remote"))
        driver.set_window_rect(1100, 800)
        driver.command("POST", "/url", {"url": base_url + "/tests/browser/robot-control.html"})
        deadline = time.monotonic() + 10
        while not _execute(driver, "return Boolean(window.robotControlHarness)"):
            if time.monotonic() > deadline:
                raise AssertionError("fixture did not load")
            time.sleep(0.05)
        _execute(driver, "window.robotControlHarness.showGroups(); return true")

        def click(selector: str) -> None:
            deadline = time.monotonic() + 5
            while not _execute(driver, f"return Boolean(document.querySelector({selector!r}))"):
                if time.monotonic() > deadline:
                    raise AssertionError(selector)
                time.sleep(0.025)
            element = driver.command("POST", "/element", {"using": "css selector", "value": selector})
            driver.command("POST", f"/element/{element['element-6066-11e4-a52e-4f735466cecf']}/click", {})

        click('.transport-selection summary')
        click('.transport-selection-options input[value="arm"]')
        click('.transport-selection-options input[value="grippers"]')
        click('.transport-selection summary')
        click('.transport-stop-primary')
        assert _execute(driver, "return window.robotControlHarness.groupCommands()") == [["arm", "grippers"]]
        click('.transport-selection summary')
        click('.transport-selection-options input[value="all"]')
        assert _execute(driver, "return [...document.querySelectorAll('.transport-selection-options input')].map(n=>n.checked)") == [True, False, False]
        click('.transport-selection-options input[value="all"]')
        assert _execute(driver, "return document.querySelector('.transport-stop-primary').disabled")
        print("PASS: real checkbox union, one callback, All exclusivity, empty-selection disable")
    finally:
        driver.delete_session()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--webdriver", required=True)
    parser.add_argument("--base-url", required=True)
    args = parser.parse_args()
    run(args.webdriver, args.base_url)
