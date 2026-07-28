# Architecture

## Purpose

The project is intended to explore concurrent task scheduling in C. All
architecture described here is planned and not yet implemented.

## Intended Component Boundaries

Planned boundaries include task producers, a thread-safe queue, a scheduler, a
worker pool, task execution, and metrics. Their interfaces and responsibilities
have not yet been designed.

## Planned High-Level Flow

```text
Task Producer
→ Thread-Safe Queue
→ Scheduler
→ Worker Pool
→ Task Execution
→ Metrics
```

This flow is a placeholder and may change as design decisions are made.
