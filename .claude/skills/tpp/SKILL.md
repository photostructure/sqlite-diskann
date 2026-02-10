---
name: tpp
description: Execute work on a Technical Project Plan (TPP). Reads TPP from _todo/, identifies current phase, and performs appropriate work (research, testing, design, implementation, or review).
allowed-tools: Read, Grep, Glob, Edit, Write, Bash
---

# TPP Execution Workflow

You are executing work on a Technical Project Plan (TPP). Follow this workflow:

## 1. Read the TPP

If $ARGUMENTS is provided, read `_todo/$ARGUMENTS.md`. Otherwise, list available TPPs in `\_todo/` and ask which one to work on.

## 2. Identify Current Phase

TPPs have 8 phases tracked with checkboxes:

- [ ] **Research & Planning**: Understand requirements, explore codebase, evaluate approaches
- [ ] **Test Design**: Plan test strategy and write test stubs
- [ ] **Implementation Design**: Design detailed implementation approach
- [ ] **Test-First Development**: Write failing tests for new functionality
- [ ] **Implementation**: Write code to pass tests
- [ ] **Integration**: Integrate with production workflow
- [ ] **Cleanup & Documentation**: Remove obsolete code, update docs
- [ ] **Final Review**: Verify all acceptance criteria

The current phase is the **first unchecked phase**. Execute work for that phase only.

## 3. Study Required Reading

Before starting work, read all files listed in the TPP's "Required Reading" section. This ensures you understand existing patterns and conventions.

**CRITICAL**: The user's global CLAUDE.md says you MUST study CLAUDE.md, TDD.md, DESIGN-PRINCIPLES.md before making ANY changes. Read these project primers first!

## 4. Execute Phase-Specific Work

### Research & Planning

- Explore codebase using Glob and Grep
- Read relevant files to understand current implementation
- Document findings in the TPP's "Tribal Knowledge" section
- Evaluate alternative approaches in "Solutions" section
- Check this phase when research is complete

### Test Design

- Plan what needs testing (units, integration, edge cases)
- Document test strategy in TPP
- Create test file stubs if helpful
- Check this phase when test plan is clear

### Implementation Design

- Design detailed implementation approach
- Identify which files need changes
- Document design decisions in TPP
- Check this phase when design is approved

### Test-First Development

- Write failing tests based on test design
- Run tests to verify they fail for the right reasons
- Document test commands in TPP's "Tasks" section
- Check this phase when tests are written and failing

### Implementation

- Write code to pass the tests
- Follow existing patterns from required reading
- Run tests frequently to verify progress
- Check this phase when all tests pass

### Integration

- Ensure code is used in production workflow (no "shelf-ware")
- Update configurations, imports, exports as needed
- Verify end-to-end functionality
- Check this phase when integrated

### Cleanup & Documentation

- Remove obsolete code
- DRY up duplicated logic
- Update documentation (avoid "lava flow" edits)
- Add comments only where logic isn't self-evident
- Check this phase when cleanup is complete

### Final Review

- Run all relevant tests (unit, integration, system)
- Verify acceptance criteria from TPP
- Check test coverage for new code
- Check this phase when everything passes

## 5. Update the TPP

After completing work:

- Check off the completed phase with `[x]`
- Add any new discoveries to "Tribal Knowledge"
- Document any failed approaches
- Update task status and verification commands

## 6. Communicate Progress

Tell the user:

- Which phase you completed
- Key findings or decisions
- What's next (the next unchecked phase)
- Whether you need user input before proceeding

## Important Guidelines

- **One phase at a time**: Don't skip ahead
- **Test-first**: Tests must be written before implementation
- **Follow patterns**: Use existing codebase conventions
- **Document failures**: Record what didn't work and why
- **Stay focused**: TPP describes the scope - don't expand it
- **Ask questions**: If requirements are unclear, ask the user

## If TPP is Complete

If all 8 phases are checked:

1. Verify all acceptance criteria are met
2. Ask user if TPP should be moved to `_done/`
3. Celebrate! This was complex work.

## Example Invocation

```
/tpp 20250209-ann-search
```

Reads `_todo/20250209-ann-search.md`, identifies current phase, and executes appropriate work.
