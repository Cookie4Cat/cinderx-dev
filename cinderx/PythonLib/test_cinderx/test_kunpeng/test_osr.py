import pytest

from test_cinderx.test_kunpeng.child_cases.osr import (
    SCENARIOS,
    _assert_scenario_output,
    _run_scenario,
)


@pytest.mark.parametrize("scenario", SCENARIOS, ids=lambda scenario: scenario.name)
def test_osr_scenarios(scenario):
    output = _run_scenario(scenario)
    _assert_scenario_output(scenario, output)
