# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Guard the guard.

Two jobs. The first is the gate itself: every baseline the repository commits is checked
here, so a result file that stops supporting the claim `benchmarks/README.md` makes about
it fails the ordinary test run rather than waiting for someone to notice.

The second is the harder one. An invariant that cannot fail is decoration, and a check
written from the same numbers it validates is exactly the kind that quietly cannot fail.
So most of what follows takes a real baseline, breaks one thing in it the way a genuine
regression would, and asserts the checker says so and names the right invariant. The
mutations are deliberately the real historical shapes: `createSession` going back to a
full-table purge, interest management silently publishing the whole world, the contended
SQLite writer drifting toward the busy timeout.
"""

from __future__ import annotations

import copy
import json

import pytest

import baselines


# --------------------------------------------------------------------------------------
# The gate: the committed baselines themselves
# --------------------------------------------------------------------------------------


def committed():
    paths = baselines.committed_baselines()
    assert paths, "no committed baselines found; results/ should not be empty"
    return paths


@pytest.mark.parametrize("path", committed(), ids=lambda p: p.name)
def test_every_committed_baseline_supports_its_claims(path):
    report = baselines.check_file(path)
    assert report.ok, "\n".join(f"{c.name}: {c.detail}" for c in report.failures)


@pytest.mark.parametrize("path", committed(), ids=lambda p: p.name)
def test_every_committed_baseline_flattens_to_comparable_metrics(path):
    metrics = baselines.flatten(baselines.load(path))
    assert metrics, f"{path.name} produced no comparable metrics"
    for key, metric in metrics.items():
        assert key == metric.key
        assert isinstance(metric.value, float)


def test_every_benchmark_in_the_tree_has_invariants():
    """A new harness must bring its claims with it, or `check` would pass it vacuously."""
    kinds = {baselines.kind_of(baselines.load(path)) for path in committed()}
    missing = kinds - set(baselines.INVARIANTS)
    assert not missing, f"no invariants defined for: {sorted(missing)}"


# --------------------------------------------------------------------------------------
# Fixtures: a real baseline, loaded fresh so a mutation cannot leak between tests
# --------------------------------------------------------------------------------------


def load_kind(kind):
    for path in committed():
        document = baselines.load(path)
        if baselines.kind_of(document) == kind:
            return document
    pytest.skip(f"no committed `{kind}` baseline to mutate")


def failures_of(document):
    return {check.name for check in baselines.check_document(document).failures}


def assert_fails(document, invariant):
    names = failures_of(document)
    assert invariant in names, f"expected `{invariant}` to fail; got {sorted(names) or 'nothing'}"


# --------------------------------------------------------------------------------------
# Mutations: the regressions these invariants exist to catch
# --------------------------------------------------------------------------------------


def test_a_full_table_purge_on_create_is_caught():
    """The real one. `createSession()` used to walk every live session, costing 306 us at
    100k against 600 ns now. Putting that back must fail the amortized-O(1) claim."""
    document = load_kind("sessions")
    biggest = max(document["sweep"], key=lambda row: row["sessions"])
    biggest["create_ns"] = 306_000.0
    assert_fails(document, "sessions.create_is_amortized_constant_time")


def test_lookup_degrading_to_a_scan_is_caught():
    document = load_kind("sessions")
    biggest = max(document["sweep"], key=lambda row: row["sessions"])
    biggest["lookup_hit_ns"] *= 50
    assert_fails(document, "sessions.lookup_stays_constant_time")


def test_interest_management_publishing_the_whole_world_is_caught():
    """If the per-session slice starts tracking N again, the arena's scaling story is
    gone; the payload claim is exact arithmetic, so this holds on any machine."""
    document = load_kind("fanout")
    for row in document["sweep"]:
        if row["mode"] == "per_session_interest":
            row["rows_per_session"] = row["consumers"]
    assert_fails(document, "fanout.interest_management_holds_the_slice_flat")


def test_interest_costing_more_than_naive_is_caught():
    document = load_kind("fanout")
    for row in document["sweep"]:
        if row["mode"] == "per_session_interest":
            row["publish_cpu"]["p50"] *= 100
    names = failures_of(document)
    assert any(name.startswith("fanout.interest_is_cheaper_to_publish") for name in names)


def test_a_contended_writer_drifting_toward_the_busy_timeout_is_caught():
    document = load_kind("persistence")
    contended = baselines._by_name(document["latency"], "sqlite_write_contended")
    contended["max"] = 4800.0
    assert_fails(document, "persistence.contended_writer_stays_far_under_the_busy_timeout")


def test_contention_moving_the_median_is_caught():
    document = load_kind("persistence")
    contended = baselines._by_name(document["latency"], "sqlite_write_contended")
    contended["p50"] *= 10
    contended["p95"] = contended["p99"] = contended["max"] = contended["p50"] * 2
    assert_fails(document, "persistence.contention_does_not_move_the_median")


def test_batching_losing_its_advantage_is_caught():
    document = load_kind("persistence")
    batched = baselines._by_name(document["scalars"], "sqlite_write_batched_rate")
    autocommit = baselines._by_name(document["scalars"], "sqlite_write_autocommit_rate")
    batched["value"] = autocommit["value"]
    assert_fails(document, "persistence.batching_beats_autocommit")


def test_a_round_trip_cheaper_than_a_one_way_push_is_caught():
    """Physically impossible on the same link, so this catches a harness that has started
    measuring something other than what it names."""
    document = load_kind("transport")
    push = baselines._by_name(document["latency"], "property_push_propagation")
    rtt = baselines._by_name(document["latency"], "slot_round_trip_64B")
    push["p50"] = rtt["p50"] * 2
    push["p95"] = push["p99"] = push["max"] = push["p50"] * 2
    assert_fails(document, "transport.one_way_cheaper_than_round_trip[push]")


def test_calls_that_stopped_pipelining_are_caught():
    document = load_kind("transport")
    rtt = baselines._by_name(document["latency"], "slot_round_trip_64B")
    throughput = baselines._by_name(document["throughput"], "slot_throughput_64B")
    throughput["value"] = 1000.0 / rtt["p50"] / 2
    assert_fails(document, "transport.calls_pipeline")


def test_payload_size_starting_to_dominate_is_caught():
    document = load_kind("transport")
    big = baselines._by_name(document["latency"], "slot_round_trip_4096B")
    for field in ("p50", "p95", "p99", "max", "mean", "min"):
        big[field] *= 10
    assert_fails(document, "transport.payload_size_barely_matters")


def test_the_mutual_tls_setup_advantage_disappearing_is_caught():
    """The measured setup gap is the entire justification for keeping `transport: local`
    as an explicit opt-in. If it closes, the config knob has stopped paying for itself."""
    document = load_kind("mesh")
    local = baselines._by_name(document["latency"], "local_socket.connection_setup")
    mtls = baselines._by_name(document["latency"], "mtls_loopback.connection_setup")
    local["p50"] = mtls["p50"]
    assert_fails(document, "mesh.local_setup_is_cheaper_than_mtls")


def test_per_message_crypto_cost_ballooning_is_caught():
    document = load_kind("mesh")
    mtls = baselines._by_name(document["latency"], "mtls_loopback.slot_round_trip_64B")
    for field in ("p50", "p95", "p99", "max", "mean", "min"):
        mtls[field] *= 20
    assert_fails(document, "mesh.steady_state_mtls_overhead_is_small")


def test_a_payload_above_the_interest_cap_is_caught():
    document = load_kind("capstone")
    document["sweep"][-1]["rows_per_session"] = document["interest_k"] + 1
    assert_fails(document, "capstone.payload_never_exceeds_the_interest_cap")


def test_a_build_that_rebuilds_everything_on_a_no_op_is_caught():
    """The defect this invariant was written from, and which it found on its first run:
    codegen writes at CMake configure time, so a generated header rewritten unconditionally
    moved its timestamp and invalidated every translation unit that included it. A no-op
    build cost 72% of a clean one. Nothing else in the repository could see that; a build
    system that recompiles the world still passes every correctness test there is."""
    document = load_kind("buildtime")
    for row in document["sweep"]:
        row["noop_s"] = row["clean_s"] * 0.72
    names = failures_of(document)
    assert any(name.startswith("buildtime.the_build_is_incremental") for name in names)


def test_a_no_op_build_that_recompiles_is_caught_before_the_wider_band_notices():
    """The band above is 50%, which a full recompile can pass. This is the measured
    regression the generator actually had: `synqt build` rewrote every `main.cpp` on every
    invocation, identical content and all, and a rewritten file has a new modification
    time whatever its bytes say. That put a no-op at ~30% of a clean build, under the 50%
    band and doing the entire compile. Content-addressed generation (synqt.writer) took it
    to well under 1%, so 30% has to be a failure and not a pass."""
    document = load_kind("buildtime")
    for row in document["sweep"]:
        row["noop_s"] = row["clean_s"] * 0.30
    names = failures_of(document)
    assert any(name.startswith("buildtime.a_no_op_build_compiles_nothing") for name in names)
    # And the wider band still says nothing, which is the point of adding a second one.
    assert not any(
        name.startswith("buildtime.the_build_is_incremental") for name in names
    )


def test_one_edit_rebuilding_everything_is_caught():
    document = load_kind("buildtime")
    for row in document["sweep"]:
        row["touched_s"] = row["clean_s"] * 0.9
    names = failures_of(document)
    assert any(
        name.startswith("buildtime.one_edit_does_not_rebuild_everything") for name in names
    )


def test_codegen_growing_into_a_real_cost_is_caught():
    document = load_kind("buildtime")
    generation = baselines._by_name(document["latency"], "contract_generation")
    slowest = max(row["clean_s"] for row in document["sweep"])
    for field in ("p50", "p95", "p99", "max", "mean", "min"):
        generation[field] = slowest * 1000.0 * 0.5
    assert_fails(document, "buildtime.codegen_is_a_rounding_error")


def test_errors_under_http_load_are_caught():
    document = load_kind("edge-http")
    document["results"][0]["errors"] = 3
    assert_fails(document, "edge-http.no_errors_under_load")


def test_bundle_totals_that_do_not_add_up_are_caught():
    document = load_kind("client-bundle")
    document["total_gzip"] += 1
    assert_fails(document, "client-bundle.totals_match_the_files")


def test_an_asset_that_compression_makes_bigger_is_caught():
    document = load_kind("client-bundle")
    document["files"][0]["brotli"] = document["files"][0]["raw"] + 1
    assert_fails(document, "client-bundle.compression_helps_every_asset")


def test_a_remote_pages_saving_that_is_not_the_difference_is_caught():
    document = load_kind("remote-pages")
    document["saving"]["brotli_bytes"] += 1000
    assert_fails(document, "remote-pages.saving_is_the_difference_it_claims")


def test_a_threaded_kit_that_lost_cross_origin_isolation_is_caught():
    """Without SharedArrayBuffer the multi-threaded bundle runs single-threaded, and its
    frame times would be filed under the wrong label."""
    for path in committed():
        document = baselines.load(path)
        if baselines.kind_of(document) == "client-frame-time" and "multi" in document["label"]:
            document["cross_origin_isolated"] = False
            assert_fails(document, "client-frame-time.threaded_kit_was_cross_origin_isolated")
            return
    pytest.skip("no multi-threaded frame-time baseline committed")


# --------------------------------------------------------------------------------------
# The universal checks
# --------------------------------------------------------------------------------------


def test_percentiles_that_do_not_ascend_are_caught():
    document = load_kind("transport")
    document["latency"][0]["p99"] = document["latency"][0]["p50"] / 2
    assert_fails(document, "distributions")


def test_a_mean_outside_its_own_range_is_caught():
    document = load_kind("transport")
    document["latency"][0]["mean"] = document["latency"][0]["max"] * 10
    assert_fails(document, "distributions")


def test_a_distribution_measured_from_no_samples_is_caught():
    document = load_kind("transport")
    document["latency"][0]["samples"] = 0
    assert_fails(document, "samples")


def test_an_unattributable_baseline_is_caught():
    document = load_kind("transport")
    del document["host"]
    assert_fails(document, "metadata")


def test_an_undated_baseline_is_a_note_and_not_a_failure():
    """Two client baselines genuinely cannot be dated. Saying so keeps them visible on
    every run without inventing a timestamp, and without failing the build over it."""
    document = load_kind("transport")
    document["recorded"] = baselines.UNKNOWN
    report = baselines.check_document(document)
    assert report.ok
    assert any(check.name == "metadata.undated" for check in report.checks)


def test_an_unrecognised_document_is_rejected_rather_than_passed():
    with pytest.raises(baselines.BaselineError):
        baselines.kind_of({"some": "json"})


def test_a_benchmark_with_no_invariants_fails_rather_than_passing_vacuously():
    report = baselines.check_document({"benchmark": "brand-new", "host": "h",
                                       "qt_version": "6.11.1", "recorded": "now"})
    assert not report.ok
    assert "invariants" in {check.name for check in report.failures}


def test_malformed_json_names_its_file(tmp_path):
    path = tmp_path / "broken.json"
    path.write_text("{not json", encoding="utf-8")
    with pytest.raises(baselines.BaselineError) as error:
        baselines.load(path)
    assert "broken.json" in str(error.value)


# --------------------------------------------------------------------------------------
# Flattening and comparison
# --------------------------------------------------------------------------------------


def test_flatten_reads_the_direction_of_each_unit():
    document = load_kind("transport")
    metrics = baselines.flatten(document)
    assert metrics["slot_round_trip_64B.p50"].lower_is_better
    assert not metrics["slot_throughput_64B"].lower_is_better


def test_flatten_names_sweep_rows_by_what_varies_not_by_index():
    """An index would silently re-point every key the moment a sweep gains a size, which
    is precisely when a comparison matters."""
    document = load_kind("fanout")
    metrics = baselines.flatten(document)
    assert any(key.startswith("per_session_interest.n") for key in metrics)
    assert not any(key.startswith("sweep.0") for key in metrics)


def test_flatten_marks_the_tail_percentiles_noisy():
    document = load_kind("transport")
    metrics = baselines.flatten(document)
    assert metrics["slot_round_trip_64B.p99"].noisy
    assert not metrics["slot_round_trip_64B.p50"].noisy


def test_a_single_outlier_moves_the_mean_but_does_not_fail_the_gate():
    """Why `mean` is ungated. The transport harness carries one ~40 ms first-sample
    outlier; halving the sample count doubles its weight in the mean while leaving every
    percentile alone. Gating the mean would report that as an 89% regression, which is
    what a real run did before this was fixed."""
    document = load_kind("transport")
    assert baselines.flatten(document)["property_push_propagation.mean"].noisy
    shorter = copy.deepcopy(document)
    push = baselines._by_name(shorter["latency"], "property_push_propagation")
    push["mean"] *= 1.89
    assert baselines.compare(document, shorter, tolerance=0.25).ok
    assert not baselines.compare(document, shorter, tolerance=0.25, strict=True).ok


def test_compare_reports_a_regression_beyond_the_tolerance():
    document = load_kind("transport")
    slower = copy.deepcopy(document)
    for field in ("p50", "p95", "p99", "max", "mean", "min"):
        slower["latency"][0][field] *= 1.5
    result = baselines.compare(document, slower, tolerance=0.25)
    assert not result.ok
    assert "slot_round_trip_64B.p50" in {delta.key for delta in result.regressions}


def test_compare_does_not_call_an_improvement_a_regression():
    document = load_kind("transport")
    faster = copy.deepcopy(document)
    for field in ("p50", "p95", "p99", "max", "mean", "min"):
        faster["latency"][0][field] /= 2
    assert baselines.compare(document, faster, tolerance=0.25).ok


def test_compare_reads_higher_is_better_units_the_right_way_round():
    """A throughput that halved has regressed even though the number went down."""
    document = load_kind("transport")
    slower = copy.deepcopy(document)
    baselines._by_name(slower["throughput"], "slot_throughput_64B")["value"] /= 2
    result = baselines.compare(document, slower, tolerance=0.25)
    assert "slot_throughput_64B" in {delta.key for delta in result.regressions}


def test_compare_leaves_the_tail_alone_unless_asked_to_be_strict():
    document = load_kind("transport")
    spiky = copy.deepcopy(document)
    spiky["latency"][0]["p99"] *= 3
    spiky["latency"][0]["max"] = spiky["latency"][0]["p99"] * 2
    assert baselines.compare(document, spiky, tolerance=0.25).ok
    assert not baselines.compare(document, spiky, tolerance=0.25, strict=True).ok


def test_compare_notices_a_metric_the_candidate_stopped_reporting():
    document = load_kind("transport")
    shrunk = copy.deepcopy(document)
    shrunk["throughput"] = []
    result = baselines.compare(document, shrunk)
    assert "slot_throughput_64B" in result.missing


# --------------------------------------------------------------------------------------
# The command line, which is what CI actually invokes
# --------------------------------------------------------------------------------------


def test_check_with_no_arguments_passes_over_the_committed_baselines(capsys):
    assert baselines.main(["check"]) == 0
    assert "baselines pass their invariants" in capsys.readouterr().out


def test_check_exits_nonzero_on_a_broken_file(tmp_path, capsys):
    document = load_kind("sessions")
    max(document["sweep"], key=lambda row: row["sessions"])["create_ns"] = 306_000.0
    path = tmp_path / "sessions-regressed.json"
    path.write_text(json.dumps(document), encoding="utf-8")
    assert baselines.main(["check", str(path)]) == 1
    assert "create_is_amortized_constant_time" in capsys.readouterr().out


def test_compare_refuses_to_diff_two_different_benchmarks(tmp_path, capsys):
    mesh = tmp_path / "mesh.json"
    transport = tmp_path / "transport.json"
    mesh.write_text(json.dumps(load_kind("mesh")), encoding="utf-8")
    transport.write_text(json.dumps(load_kind("transport")), encoding="utf-8")
    assert baselines.main(["compare", str(mesh), str(transport)]) == 1
    assert "refusing to compare" in capsys.readouterr().err


def test_compare_of_a_file_against_itself_is_clean(tmp_path, capsys):
    path = tmp_path / "transport.json"
    path.write_text(json.dumps(load_kind("transport")), encoding="utf-8")
    assert baselines.main(["compare", str(path), str(path)]) == 0
    assert "no metric regressed" in capsys.readouterr().out


def test_show_prints_every_flattened_metric(tmp_path, capsys):
    path = tmp_path / "mesh.json"
    document = load_kind("mesh")
    path.write_text(json.dumps(document), encoding="utf-8")
    assert baselines.main(["show", str(path)]) == 0
    out = capsys.readouterr().out
    assert "mtls_loopback.connection_setup.p50" in out
    assert f"{len(baselines.flatten(document))} metrics" in out


def test_compare_of_runs_with_no_metric_in_common_is_an_error(tmp_path, capsys):
    """Two sweeps over different sizes share no keys. Reporting "nothing regressed" for a
    comparison that compared nothing is the one answer that must not happen."""
    document = load_kind("sessions")
    other = copy.deepcopy(document)
    for row in other["sweep"]:
        row["sessions"] *= 7
    first, second = tmp_path / "a.json", tmp_path / "b.json"
    first.write_text(json.dumps(document), encoding="utf-8")
    second.write_text(json.dumps(other), encoding="utf-8")
    assert baselines.main(["compare", str(first), str(second)]) == 1
    assert "nothing to compare" in capsys.readouterr().err
