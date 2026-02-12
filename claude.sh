#!/bin/bash

# To make sure we use this if available:
# alias claude='if [ -f "./claude.sh" ]; then ./claude.sh; else command claude; fi'

echo "Adding sqlite-diskann system prompt..."

DATE=$(date +%Y-%m-%d)

claude --append-system-prompt "$(
  cat <<'EOF'

## Required Reading BEFORE Making Changes

- **YOU MUST STUDY** @CLAUDE.md, @TDD.md, and @DESIGN-PRINCIPLES.md before making ANY changes

- **Read the code** - Never guess at function signatures, APIs, or implementations. ALWAYS verify.

- **Check _todo/ TPPs** - If a Technical Project Plan exists for your task, FOLLOW IT.

- Never assume the codebase was clean when you started. Git status may show uncommitted changes from other sessions.

## When exiting Plan mode

If your plan is accepted, be sure to update your TPP accordingly. Follow our style guide, @TPP-GUIDE.md 

**The user will revert your work if you don't follow this step**

## `git` commit messages

- Use Conventional Commits format. See https://www.conventionalcommits.org/en/v1.0.0/
- For the "scope" of the Conventional Commit message, use the name of the file with the most important changes
- Be concise: less than 5 lines. If you can't summarize the change in 5 lines, you probably need to break it up into smaller commits.
- Do NOT include "Generated with [Claude Code]" messaging
- Do NOT include any Co-Authored-By trailers
- Do NOT include random counts of lines changed, files changed, tests added, etc.

## ALWAYS ASK before you `git commit` or `git push`

DO NOT EVER `git commit` or `git push` without asking the user first. You do not have full context of the state of the codebase.

## Concurrent Session Protocol

**Assume multiple engineers are editing the same codebase simultaneously.**

If you encounter a compilation error that you don't think you caused:
1. **STOP immediately** - Do not try to fix it blindly
2. **Describe the error** to the user clearly
3. **Use AskUserQuestion**. Present these options to the user:
   - "Build is now fixed, continue"
   - "Please fix the build and then continue"

EOF
)" "$@"
