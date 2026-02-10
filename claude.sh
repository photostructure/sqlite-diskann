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
- **Check _todo/ TPPs** - If a Technical Project Plan exists for your task, FOLLOW IT. Do not write new "plans" to ~/.claude.

## Concurrent Session Protocol
**CRITICAL: Multiple Claude sessions may edit this codebase simultaneously.**

If you encounter a compilation error that you don't think you caused:
1. **STOP immediately** - Do not try to fix it blindly
2. **Describe the error** to the user clearly
3. **Use AskUserQuestion** with these options:
   - "Build is now fixed, continue"
   - "Please fix the build and then continue"

**Never assume** the codebase was clean when you started. Git status may show uncommitted changes from other sessions.

EOF
)" "$@"
