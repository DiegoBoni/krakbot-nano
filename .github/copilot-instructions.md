# Copilot Instructions

<!-- .AIAgents Autoload Start -->
## Agent context

This project uses a shared multi-agent workflow. All agents read from the same shared artifacts:
- `.ai/project-context.md` — repository context (stack, architecture, conventions)
- `specs/<type>/<slug>/spec.md` — feature or bug specification
- `specs/<type>/<slug>/plan.md` — implementation plan
- `specs/<type>/<slug>/tasks.md` — atomic task list
- `.ai/current` — path to the active spec directory

## Domain skills (auto-discovered)

Skills are in `.github/skills/` — Copilot Agent Mode discovers and activates them automatically
when your prompt is relevant to a domain. No manual loading needed.

- `.github/skills/backend/SKILL.md`
- `.github/skills/frontend/SKILL.md`
- `.github/skills/data/SKILL.md`
- `.github/skills/testing/SKILL.md`
- `.github/skills/devops/SKILL.md`

## Workflow commands (prompt templates)

Copilot has no native slash commands. Use these prompt templates: open the file and paste into
Copilot Chat to execute the workflow step.

- `.copilot/commands/scan.md`      → profile repo and generate `.ai/project-context.md`
- `.copilot/commands/spec.md`      → define feature or bug requirements
- `.copilot/commands/plan.md`      → architecture + phased implementation plan
- `.copilot/commands/tasks.md`     → atomic task list with dependencies
- `.copilot/commands/implement.md` → execute tasks domain by domain
- `.copilot/commands/fix.md`       → minimal bug fix (add --trace for traceability)
- `.copilot/commands/status.md`    → pipeline snapshot — stage, task counts, next step
- `.copilot/commands/switch.md`    → change active spec without re-running spec

## Startup behavior (required)

1. Run the `scan` prompt first to create or update `.ai/project-context.md`.
2. Domain skills in `.github/skills/` are activated automatically in Agent Mode.
3. Each skill specifies exactly which section of `project-context.md` to read.
4. If critical info is missing, mark `NEEDS CLARIFICATION` and continue with safe defaults.

## Multi-agent workflow

- Spec phase (scan + spec + plan): best handled by an analysis-focused agent
- Implementation phase (implement): best handled by a code-generation agent
- Shared artifact: `specs/<type>/<slug>/` — any agent can pick up via `.ai/current`

## Notes

- Skills are model-agnostic — they work with any model powering Copilot (GPT-4o, Claude, Gemini, etc.)
- Skill auto-discovery requires Copilot Agent Mode (VS Code). In regular chat, paste the skill content manually.

Bootstrap command:
`./.AIAgents/scripts/bootstrap-commands.sh --repo . --agent all --mode copy`
<!-- .AIAgents Autoload End -->
