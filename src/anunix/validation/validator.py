"""Validation engine — trust scoring and output verification.

Based on RFC-0003 Section 20 and RFC-0004 Section 16.
"""

from __future__ import annotations

from enum import Enum
from typing import Any

from pydantic import BaseModel, Field

from anunix.core.types import now_utc
from anunix.state.object import StateObject, ValidationRecord, ValidationState


class ValidationOutcome(str, Enum):
    PASS = "pass"
    PASS_WITH_WARNINGS = "pass_with_warnings"
    RETRYABLE_FAIL = "retryable_fail"
    TERMINAL_FAIL = "terminal_fail"
    ESCALATE = "escalate"


class ValidationIssue(BaseModel):
    severity: str = "warning"
    code: str = ""
    message: str = ""


class ValidationResult(BaseModel):
    outcome: ValidationOutcome
    confidence: float | None = None
    new_state: ValidationState
    issues: list[ValidationIssue] = Field(default_factory=list)
    method: str = ""


class ValidationRule:
    """Base class for validation rules."""

    name: str = "base_rule"

    def check(self, obj: StateObject) -> ValidationResult:
        raise NotImplementedError


class SchemaValidationRule(ValidationRule):
    """Validates that required fields are present and correctly typed."""

    name = "schema_check"

    def __init__(self, required_fields: list[str] | None = None) -> None:
        self._required = required_fields or []

    def check(self, obj: StateObject) -> ValidationResult:
        issues: list[ValidationIssue] = []

        if not obj.type:
            issues.append(ValidationIssue(
                severity="error", code="missing_type", message="Object has no type"
            ))
        if not obj.payload and not obj.payload_ref:
            issues.append(ValidationIssue(
                severity="warning", code="empty_payload", message="Object has no payload"
            ))
        for field in self._required:
            if field not in obj.metadata:
                issues.append(ValidationIssue(
                    severity="error",
                    code=f"missing_{field}",
                    message=f"Required metadata field '{field}' is missing",
                ))

        if any(i.severity == "error" for i in issues):
            return ValidationResult(
                outcome=ValidationOutcome.TERMINAL_FAIL,
                new_state=ValidationState.REJECTED,
                issues=issues,
                method=self.name,
            )
        if issues:
            return ValidationResult(
                outcome=ValidationOutcome.PASS_WITH_WARNINGS,
                new_state=ValidationState.PROVISIONAL,
                issues=issues,
                method=self.name,
            )
        return ValidationResult(
            outcome=ValidationOutcome.PASS,
            new_state=ValidationState.PROVISIONAL,
            method=self.name,
        )


class ConfidenceThresholdRule(ValidationRule):
    """Validates that an object meets a minimum confidence threshold."""

    name = "confidence_threshold"

    def __init__(self, threshold: float = 0.7) -> None:
        self._threshold = threshold

    def check(self, obj: StateObject) -> ValidationResult:
        confidence = obj.validation.confidence
        if confidence is None:
            return ValidationResult(
                outcome=ValidationOutcome.PASS_WITH_WARNINGS,
                new_state=ValidationState.UNVALIDATED,
                issues=[ValidationIssue(
                    severity="info",
                    code="no_confidence",
                    message="No confidence score available",
                )],
                method=self.name,
            )
        if confidence < self._threshold:
            return ValidationResult(
                outcome=ValidationOutcome.RETRYABLE_FAIL,
                confidence=confidence,
                new_state=ValidationState.CONTESTED,
                issues=[ValidationIssue(
                    severity="warning",
                    code="low_confidence",
                    message=f"Confidence {confidence:.2f} below threshold {self._threshold}",
                )],
                method=self.name,
            )
        return ValidationResult(
            outcome=ValidationOutcome.PASS,
            confidence=confidence,
            new_state=ValidationState.VALIDATED,
            method=self.name,
        )


class ProvenanceRule(ValidationRule):
    """Validates that derived objects have proper provenance."""

    name = "provenance_check"

    def check(self, obj: StateObject) -> ValidationResult:
        prov = obj.provenance
        if prov.origin_kind.value == "derived" and not prov.source_refs:
            return ValidationResult(
                outcome=ValidationOutcome.TERMINAL_FAIL,
                new_state=ValidationState.REJECTED,
                issues=[ValidationIssue(
                    severity="error",
                    code="missing_source_refs",
                    message="Derived object has no source references",
                )],
                method=self.name,
            )
        return ValidationResult(
            outcome=ValidationOutcome.PASS,
            new_state=ValidationState.PROVISIONAL,
            method=self.name,
        )


class Validator:
    """Runs a pipeline of validation rules against a State Object."""

    def __init__(self, rules: list[ValidationRule] | None = None) -> None:
        self._rules = rules or [
            SchemaValidationRule(),
            ProvenanceRule(),
        ]

    def add_rule(self, rule: ValidationRule) -> None:
        self._rules.append(rule)

    def validate(self, obj: StateObject) -> ValidationResult:
        """Run all rules and return the aggregate result."""
        all_issues: list[ValidationIssue] = []
        worst_outcome = ValidationOutcome.PASS
        final_state = ValidationState.VALIDATED
        final_confidence: float | None = None

        outcome_severity = {
            ValidationOutcome.PASS: 0,
            ValidationOutcome.PASS_WITH_WARNINGS: 1,
            ValidationOutcome.RETRYABLE_FAIL: 2,
            ValidationOutcome.ESCALATE: 3,
            ValidationOutcome.TERMINAL_FAIL: 4,
        }
        state_severity = {
            ValidationState.VALIDATED: 0,
            ValidationState.PROVISIONAL: 1,
            ValidationState.UNVALIDATED: 2,
            ValidationState.CONTESTED: 3,
            ValidationState.STALE: 4,
            ValidationState.REJECTED: 5,
        }

        for rule in self._rules:
            result = rule.check(obj)
            all_issues.extend(result.issues)

            if outcome_severity[result.outcome] > outcome_severity[worst_outcome]:
                worst_outcome = result.outcome
            if state_severity[result.new_state] > state_severity[final_state]:
                final_state = result.new_state
            if result.confidence is not None:
                final_confidence = result.confidence

        # Apply the result to the object
        obj.validation = ValidationRecord(
            state=final_state,
            confidence=final_confidence,
            methods=[{"name": r.name, "timestamp": now_utc().isoformat()} for r in self._rules],
            issues=[i.model_dump() for i in all_issues],
        )

        return ValidationResult(
            outcome=worst_outcome,
            confidence=final_confidence,
            new_state=final_state,
            issues=all_issues,
            method="aggregate",
        )
