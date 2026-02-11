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
