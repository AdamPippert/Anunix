# Anunix — Agent Native UNIX

A proposal for a next-generation UNIX-like operating system designed for both classical computing and model-based computation.

## Thesis

Traditional operating systems treat:
- files as passive data
- processes as isolated execution units
- memory as a hidden implementation detail

This architecture proposes:

- **State Objects instead of Files**
- **Execution Cells instead of Processes**
- **Semantic Streams instead of Byte Pipes**
- **Memory as a First-Class System Resource**
- **Provenance as a Default Property**

## Core Idea

Preserve the composability and minimalism of UNIX, but redefine the primitives around:

- state
- transformation
- memory
- provenance
- policy

## Structure

## Roadmap

- RFC-0001: Architecture Thesis
- RFC-0002: State Object Model
- RFC-0003: Execution Cell Runtime
- RFC-0004: Memory Control Plane
- RFC-0005: Scheduler + Model Routing

## Why This Matters

AI-native workloads require:
- semantic awareness
- recursive execution
- probabilistic computation
- memory orchestration

This repo explores how an OS should evolve to support that natively.
