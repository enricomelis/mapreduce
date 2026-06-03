---
name: mapreduce-session-start
description: Start-of-session workflow for the local mapreduce C project. Use when the programmer begins a programming session in /Users/enricomelis/code/mapreduce and needs an overview of the current project state, a check that docs/diario.md reflects recent development and architectural decisions, and a learning-oriented recall check before choosing the next implementation step.
---

# MapReduce Session Start

## Overview

Use this skill to orient a new programming session around the project truth in `docs/Testo.md`, the working notes in `docs/diario.md`, and the current repository state. Keep the interaction didactic: help the programmer explain what exists, why it exists, and what should come next.

## Project Scope

- Work only in `/Users/enricomelis/code/mapreduce`.
- Treat `docs/Testo.md` as the source of truth for the base project.
- Use `docs/diario.md` as the development diary and architectural decision log.
- Respect `AGENTS.md`: guide learning first, avoid generating code unless the user explicitly asks for a small didactic change, use `gcc`, and do not run `make`.

## Startup Workflow

1. Inspect the local context:
   - read `AGENTS.md`;
   - read the latest relevant parts of `docs/Testo.md`;
   - read `docs/diario.md`;
   - check `git status --short`;
   - inspect only the files needed to understand the current state.

2. Verify the diary:
   - confirm whether `docs/diario.md` documents the current implemented behavior, pending work, and architectural choices;
   - compare the diary with `git status` and recent code/test structure;
   - if the diary appears stale or incomplete, stop and ask whether to update it before continuing.

3. Give a concise overview:
   - what is implemented;
   - what is intentionally incomplete;
   - which constraints from `docs/Testo.md` matter now;
   - which technical decisions are already in place;
   - one or two reasonable next steps, ordered by learning value and project dependency.

4. Run a learning recall before implementation:
   - ask the programmer to explain the current design in their own words;
   - ask 2 to 4 focused questions about recent work, architectural choices, and the next task;
   - do not provide full answers before the programmer tries;
   - after the answer, correct gaps briefly and tie the correction back to `docs/Testo.md`.

5. Choose the next work item:
   - prefer a small, explainable step;
   - state the intended behavioral change before any code edit;
   - avoid broad refactors unless they are necessary for the next requirement.

## Output Shape

Use short sections:
- `Stato`: current implementation and documentation status.
- `Da Verificare`: any mismatch between code, tests, and diary.
- `Recall`: questions for the programmer.
- `Prossimo Passo`: one recommended task and why it is the right next step.
