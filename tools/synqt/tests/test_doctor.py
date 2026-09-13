# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt doctor` on a project it has to diagnose rather than build."""

from synqt import doctor


def test_doctor_reports_on_a_provider_written_as_a_bare_name(tmp_path):
    # `synqt doctor` is what someone runs when something is wrong, so it reads a misshaped
    # entity and reports, rather than stopping with a traceback.
    (tmp_path / "synqt.yaml").write_text(
        "entities:\n  - name: store\n    type: relational\n    provider: postgres\n"
        "  - not an entity\n")
    assert "synqt doctor:" in doctor.report(tmp_path)
