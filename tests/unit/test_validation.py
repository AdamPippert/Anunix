"""Tests for the Validation layer."""

from anunix.state.object import (
    OriginKind,
    ProvenanceEntry,
    StateObject,
    ValidationRecord,
    ValidationState,
)
from anunix.validation.validator import (
    ConfidenceThresholdRule,
    ProvenanceRule,
    SchemaValidationRule,
    ValidationOutcome,
    Validator,
)


def test_schema_validation_pass():
    rule = SchemaValidationRule()
    obj = StateObject(type="document.markdown", payload="# Hello")
    result = rule.check(obj)
    assert result.outcome == ValidationOutcome.PASS


def test_schema_validation_missing_required_field():
    rule = SchemaValidationRule(required_fields=["language"])
    obj = StateObject(type="document.transcript", payload="text")
    result = rule.check(obj)
    assert result.outcome == ValidationOutcome.TERMINAL_FAIL
    assert any("language" in i.message for i in result.issues)


def test_schema_validation_empty_payload_warning():
    rule = SchemaValidationRule()
    obj = StateObject(type="document.markdown")
    result = rule.check(obj)
    assert result.outcome == ValidationOutcome.PASS_WITH_WARNINGS
    assert result.issues[0].code == "empty_payload"


def test_confidence_threshold_pass():
    rule = ConfidenceThresholdRule(threshold=0.7)
    obj = StateObject(
        type="memory.summary",
        validation=ValidationRecord(confidence=0.9),
    )
    result = rule.check(obj)
    assert result.outcome == ValidationOutcome.PASS
    assert result.new_state == ValidationState.VALIDATED


def test_confidence_threshold_fail():
    rule = ConfidenceThresholdRule(threshold=0.8)
    obj = StateObject(
        type="memory.summary",
        validation=ValidationRecord(confidence=0.5),
    )
    result = rule.check(obj)
    assert result.outcome == ValidationOutcome.RETRYABLE_FAIL
    assert result.new_state == ValidationState.CONTESTED


def test_confidence_threshold_no_score():
    rule = ConfidenceThresholdRule()
    obj = StateObject(type="memory.summary")
    result = rule.check(obj)
    assert result.outcome == ValidationOutcome.PASS_WITH_WARNINGS


def test_provenance_rule_derived_without_sources():
    rule = ProvenanceRule()
    obj = StateObject(
        type="memory.summary",
        provenance=ProvenanceEntry(origin_kind=OriginKind.DERIVED, source_refs=[]),
    )
    result = rule.check(obj)
    assert result.outcome == ValidationOutcome.TERMINAL_FAIL
    assert result.new_state == ValidationState.REJECTED


def test_provenance_rule_derived_with_sources():
    rule = ProvenanceRule()
    from anunix.core.types import ObjectID
    obj = StateObject(
        type="memory.summary",
        provenance=ProvenanceEntry(
            origin_kind=OriginKind.DERIVED,
            source_refs=[ObjectID("so_parent123")],
        ),
    )
    result = rule.check(obj)
    assert result.outcome == ValidationOutcome.PASS


def test_validator_aggregate():
    validator = Validator(rules=[
        SchemaValidationRule(),
        ProvenanceRule(),
    ])
    obj = StateObject(type="document.transcript", payload="text content")
    result = validator.validate(obj)
    assert result.outcome == ValidationOutcome.PASS
    assert obj.validation.state == ValidationState.PROVISIONAL


def test_validator_applies_to_object():
    validator = Validator(rules=[
        SchemaValidationRule(),
        ConfidenceThresholdRule(threshold=0.7),
    ])
    obj = StateObject(
        type="memory.fact_candidate",
        payload="The sky is blue",
        validation=ValidationRecord(confidence=0.85),
    )
    result = validator.validate(obj)
    assert result.outcome == ValidationOutcome.PASS
    # Schema rule yields PROVISIONAL, confidence yields VALIDATED; worst wins
    assert obj.validation.state == ValidationState.PROVISIONAL
    assert obj.validation.confidence == 0.85
    assert len(obj.validation.methods) == 2


def test_validator_worst_outcome_wins():
    validator = Validator(rules=[
        SchemaValidationRule(),
        ConfidenceThresholdRule(threshold=0.9),
    ])
    obj = StateObject(
        type="memory.summary",
        payload="summary text",
        validation=ValidationRecord(confidence=0.6),
    )
    result = validator.validate(obj)
    assert result.outcome == ValidationOutcome.RETRYABLE_FAIL
    assert obj.validation.state == ValidationState.CONTESTED
