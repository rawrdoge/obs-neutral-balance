---
id: 3
title: Architecture & Contributor Documentation Structure
type: task
status: open
blocked_by: []
---

## Question

What documentation files (`CONTRIBUTING.md`, `ARCHITECTURE.md`, `DEVELOPMENT.md`) need to be added to lower the barrier for external contributors to understand the color pipeline, build requirements, and PR guidelines?

## Spec & Requirements to Resolve
1. Outline `CONTRIBUTING.md`: PR process, coding standards (C17, clang-format rules), localized strings guidelines (`en-US.ini`).
2. Outline `ARCHITECTURE.md`: High-level diagram and textual explanation of the 3-stage shader pipeline (inverse EOTF -> 3x3 CAT matrix -> forward EOTF) and thread model (render thread vs async stats capture thread vs Qt UI).
