"""Exception hierarchy for the Anunix system."""


class AnunixError(Exception):
    """Base exception for all Anunix errors."""


# State Object errors
class StateObjectError(AnunixError):
    """Error related to State Object operations."""


class ObjectNotFoundError(StateObjectError):
    """Requested State Object does not exist."""


class ObjectValidationError(StateObjectError):
    """State Object failed validation."""


class PolicyViolationError(StateObjectError):
    """Operation violates the object's policy."""


# Execution errors
class ExecutionError(AnunixError):
    """Error related to Execution Cell operations."""


class CellAdmissionError(ExecutionError):
    """Cell failed admission policy checks."""


class CellTimeoutError(ExecutionError):
    """Cell execution exceeded timeout."""


class DecompositionOverflowError(ExecutionError):
    """Cell decomposition exceeded depth or fan-out limits."""


# Routing errors
class RoutingError(AnunixError):
    """Error related to routing decisions."""


class NoValidRouteError(RoutingError):
    """No feasible route exists for the given task and constraints."""


class EngineUnavailableError(RoutingError):
    """Selected engine is not available."""


# Memory errors
class MemoryError(AnunixError):
    """Error related to Memory Control Plane operations."""


class AdmissionDeniedError(MemoryError):
    """Object was denied memory admission."""


class PromotionDeniedError(MemoryError):
    """Object does not meet promotion requirements."""


# Network errors
class NetworkError(AnunixError):
    """Error related to Network Plane operations."""


class PeerUnreachableError(NetworkError):
    """Remote peer is not reachable."""


class TrustZoneViolationError(NetworkError):
    """Operation violates trust zone policy."""


class ReconciliationConflictError(NetworkError):
    """Reconciliation encountered unresolvable conflicts."""
