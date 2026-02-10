# Technical Project Plan (TPP) System Guide

## What is a TPP?

A **Technical Project Plan (TPP)** is a structured markdown document that guides complex engineering work across multiple Claude Code sessions. Think of it as **institutional memory** - when your context window fills up, the TPP ensures the next session knows everything you learned.

### Why Use TPPs?

**Context windows fill up.** On complex features, you'll hit limits before finishing. Rather than using `/compact` (which often loses important details), TPPs preserve:

- **Decisions made** - Why approach X over Y
- **Failures documented** - What didn't work and why
- **Tribal knowledge** - Non-obvious gotchas
- **Clear next steps** - Where to resume

A quality TPP reads like **notes from the previous engineer** - answering the "why" behind decisions, not just listing instructions.

## TPP Lifecycle

```
Create → Research → Design → Implement → Review → Complete → Archive
  ↓         ↓         ↓          ↓          ↓         ↓         ↓
_todo/   _todo/    _todo/     _todo/     _todo/    _todo/    _done/
```

### File Naming

**Active TPPs:** `_todo/YYYYMMDD-feature-name.md`
**Completed TPPs:** `_done/YYYYMMDD-feature-name.md`

Date prefix ensures chronological sorting. Feature name should be concise (3-5 words max).

Examples:

- `_todo/20250209-hnsw-index.md`
- `_todo/20250209-vector-normalization.md`
- `_done/20250208-sqlite-extension.md`

## TPP Structure

Every TPP follows this template:

````markdown
# Feature Name

## Summary

Brief problem description (under 10 lines)

## Current Phase

- [ ] Research & Planning
- [ ] Test Design
- [ ] Implementation Design
- [ ] Test-First Development
- [ ] Implementation
- [ ] Integration
- [ ] Cleanup & Documentation
- [ ] Final Review

## Required Reading

- `CLAUDE.md` - Project conventions
- `TDD.md` - Testing methodology
- `DESIGN-PRINCIPLES.md` - C coding standards
- `src/relevant_file.c` - Existing implementation

## Description

Detailed context (under 20 lines)

- What problem are we solving?
- What are the constraints?
- What's the desired outcome?

## Tribal Knowledge

Non-obvious details, gotchas, historical context

- Vector normalization is critical - segfaults otherwise
- SQLite's vector extension loads lazily
- Distance threshold 0.15 gives best precision/recall

## Solutions

Evaluated alternatives with pros/cons

### Option 1: HNSW Algorithm

**Pros:** Fast queries, good recall
**Cons:** High memory usage
**Status:** Chosen approach

### Option 2: LSH (Locality-Sensitive Hashing)

**Pros:** Lower memory
**Cons:** Lower recall, complex tuning
**Status:** Rejected

## Tasks

- [ ] Implement HNSW graph construction
- [ ] Add KNN search function
- [ ] Write unit tests
- [ ] Benchmark against brute force
- [ ] Integrate with SQLite extension

**Verification:**

```bash
make test
./benchmark 10000 128
```
````

## Notes

Session-specific findings, progress updates

````

## The 8 Phases

### 1. Research & Planning ⚙️
**Goal:** Understand the problem and codebase

**Activities:**
- Explore relevant code with Grep/Glob
- Read existing implementations
- Document current state in TPP
- Evaluate alternative approaches

**Output:**
- Updated "Tribal Knowledge" section
- Filled "Solutions" section with pros/cons
- Clear understanding of scope

**When to check:** When you can articulate the problem and potential solutions

---

### 2. Test Design 🧪
**Goal:** Plan what needs testing

**Activities:**
- Identify test cases (happy path, edge cases, errors)
- Plan test data fixtures
- Document test strategy in TPP
- Create test file stubs if helpful

**Output:**
- Test plan documented
- Test file structure created
- No implementation yet

**When to check:** When you know what tests to write

---

### 3. Implementation Design 📐
**Goal:** Design the detailed approach

**Activities:**
- Sketch data structures
- Design function signatures
- Identify files needing changes
- Document design decisions

**Output:**
- Clear implementation plan
- Function signatures defined
- Architecture decisions documented

**When to check:** When you can explain the implementation approach to another engineer

---

### 4. Test-First Development ✅
**Goal:** Write failing tests

**Activities:**
- Write tests based on test design
- Run tests to verify they fail correctly
- Document test commands in Tasks section

**Output:**
- Tests written and failing
- Clear error messages showing what's missing

**When to check:** When tests fail for the right reasons

---

### 5. Implementation 💻
**Goal:** Make tests pass

**Activities:**
- Write code following DESIGN-PRINCIPLES.md
- Run tests frequently
- Keep changes focused on current TPP scope

**Output:**
- Tests passing
- Code follows project conventions
- Memory-safe (Valgrind clean)

**When to check:** When all tests pass

---

### 6. Integration 🔗
**Goal:** Connect to production workflow

**Activities:**
- Ensure code is actually used (no "shelf-ware")
- Update configurations, exports, imports
- Verify end-to-end functionality

**Output:**
- Feature accessible in production code
- No dead/unused code

**When to check:** When feature works in real usage scenario

---

### 7. Cleanup & Documentation 🧹
**Goal:** Leave code better than you found it

**Activities:**
- Remove obsolete code
- DRY up duplicated logic
- Update documentation
- Add comments only where needed

**Output:**
- No technical debt left behind
- Documentation accurate
- Code simplified

**When to check:** When codebase is cleaner

---

### 8. Final Review ✓
**Goal:** Verify everything works

**Activities:**
- Run all relevant tests (unit, integration, system)
- Check test coverage
- Verify acceptance criteria
- Run Valgrind and sanitizers

**Output:**
- All tests passing
- Coverage >80%
- No memory leaks
- Acceptance criteria met

**When to check:** When ready to ship

## Using the `/tpp` Skill

The `/tpp` skill automates TPP execution:

```bash
# Work on a specific TPP
/tpp 20250209-hnsw-index

# If no argument, lists available TPPs and prompts
/tpp
````

**What it does:**

1. Reads the TPP file
2. Identifies current phase (first unchecked box)
3. Studies required reading
4. Executes phase-appropriate work
5. Updates TPP with findings
6. Checks off completed phase
7. Reports progress

**You don't need to:**

- Remember what phase you're on
- Re-read all the context
- Figure out what to do next

## Using the `/handoff` Skill

When context is 80-90% full or switching tasks:

```bash
/handoff 20250209-hnsw-index
```

**What it does:**

1. Reviews conversation for discoveries
2. Updates "Tribal Knowledge" with learnings
3. Documents failed approaches
4. Updates task status
5. Marks completed phases
6. States clear blockers if stuck

**Use `/handoff` when:**

- Context window approaching limit
- Switching to different task
- Major discovery changes the plan
- Session ending for the day
- Hit a blocker that needs user input

## Best Practices

### Keep TPPs Focused

- One feature per TPP
- Under 400 lines total
- Specific, bounded scope
- If scope grows, split into multiple TPPs

### Document Failures

```markdown
## Tribal Knowledge

**What didn't work:**

- Tried brute-force search first - O(n²) too slow
- Attempted SIMD distance calc - compiler issues on ARM
- LSH needed too much parameter tuning

**Why current approach:**

- HNSW provides good speed/recall tradeoff
- Mature implementation to reference
- Fits in memory constraints
```

### Be Specific in Tribal Knowledge

```markdown
<!-- Bad -->

Be careful with vectors

<!-- Good -->

Vector normalization is critical - SQLite's ann extension
segfaults on non-normalized vectors. Always call normalize_vector()
before insertion. Found this after 2 hours of debugging.
```

### Update as You Learn

TPPs are **living documents** during active work:

- Add discoveries immediately
- Update task status as you go
- Check off phases when complete
- Don't wait until handoff

### Size Control

- **Summary:** <10 lines
- **Description:** <20 lines
- **Total TPP:** <400 lines (500-line buffer before truncation)

If approaching limits:

- Move detailed notes to separate docs
- Reference external files
- Keep TPP as navigation hub

## Example TPP Workflow

### Day 1: Starting Fresh

```bash
# Create TPP
cat > _todo/20250209-ann-search.md << 'EOF'
# ANN Search Implementation

## Summary
Add approximate nearest neighbor search to sqlite-ann extension.

## Current Phase
- [ ] Research & Planning
...
EOF

# Start work
/tpp 20250209-ann-search
```

Claude reads TPP, sees "Research & Planning" unchecked, explores codebase, documents findings, checks off phase.

### Day 2: Continuing Work

```bash
# Context at 60% - continue same session
/tpp 20250209-ann-search
```

Claude reads TPP, sees "Test Design" unchecked, plans tests, documents strategy, checks off phase.

### Day 3: Context Full

```bash
# Context at 90% - need handoff
/handoff 20250209-ann-search
```

Claude updates TPP with discoveries, marks completed phases, documents blockers. Start fresh session:

```bash
# New session
/tpp 20250209-ann-search
```

Claude reads updated TPP, continues from "Implementation" phase with full context.

### Day 4: Complete

```bash
/tpp 20250209-ann-search
```

Claude completes final review, all 8 phases checked. Prompts to move to `_done/`:

```bash
mv _todo/20250209-ann-search.md _done/
```

## Creating Your First TPP

### 1. Identify Scope

Not every task needs a TPP. Use TPPs for:

- Complex features (>3 files changed)
- Unfamiliar code areas
- Work spanning multiple sessions
- High-risk changes

### 2. Create File

```bash
# Today's date + feature name
touch _todo/20250209-feature-name.md
```

### 3. Fill Template

Use structure above. Key sections:

- **Summary** - 1-2 sentences
- **Description** - More detail, constraints, goals
- **Required Reading** - What to study first
- **Tasks** - Specific deliverables

### 4. Start Work

```bash
/tpp 20250209-feature-name
```

### 5. Update Throughout

Don't wait for handoff - update as you learn:

- Check off completed phases
- Add tribal knowledge discoveries
- Update task status
- Document failed approaches

### 6. Handoff When Needed

```bash
/handoff 20250209-feature-name
```

### 7. Complete and Archive

When all 8 phases checked:

```bash
mv _todo/20250209-feature-name.md _done/
```

## TPP Template

Copy this template for new TPPs:

````markdown
# Feature Name

## Summary

(Under 10 lines - what problem are we solving?)

## Current Phase

- [ ] Research & Planning
- [ ] Test Design
- [ ] Implementation Design
- [ ] Test-First Development
- [ ] Implementation
- [ ] Integration
- [ ] Cleanup & Documentation
- [ ] Final Review

## Required Reading

- `CLAUDE.md` - Project conventions
- `TDD.md` - Testing methodology
- `DESIGN-PRINCIPLES.md` - C coding standards
- (Add relevant source files)

## Description

(Under 20 lines - detailed context)

- **Problem:** What are we solving?
- **Constraints:** What are the limitations?
- **Success Criteria:** How do we know it's done?

## Tribal Knowledge

(Non-obvious details discovered during work)

## Solutions

(Evaluated alternatives)

### Option 1: Approach Name

**Pros:**
**Cons:**
**Status:** [Chosen/Rejected/Investigating]

## Tasks

- [ ] Task 1
- [ ] Task 2
- [ ] Task 3

**Verification:**

```bash
# Commands to verify completion
make test
```
````

## Notes

(Session-specific findings)

````

## Advanced Tips

### Linking TPPs
When one TPP depends on another:
```markdown
## Blockers
Blocked by: `20250208-vector-ops.md` (must complete first)
````

### Splitting Large TPPs

If scope grows too large:

```markdown
# Original TPP becomes parent

## Related TPPs

- `20250210-ann-search-hnsw.md` - HNSW implementation
- `20250211-ann-search-benchmarks.md` - Performance testing
```

### Template Variations

Adjust template for project needs:

- Add "Performance Requirements" section
- Add "Security Considerations" section
- Add "API Design" section for libraries

### Auto-Handoff Reminder

Set a personal reminder at 80% context:

```markdown
<!-- In your personal CLAUDE.md -->

When context reaches 80%, proactively suggest using /handoff
```

## Troubleshooting

### "TPP is too long"

- Move detailed notes to separate files
- Reference docs instead of copying
- Keep TPP as navigation hub

### "Forgot to update TPP"

- Use `/handoff` to batch update
- Review conversation history
- Document what you remember

### "Don't know current phase"

- Read the TPP - first unchecked box
- `/tpp` will identify it for you

### "Hit a blocker"

```markdown
## BLOCKER

Cannot proceed until:

- [ ] User provides decision on X
- [ ] Upstream bug fixed

Next session: address blocker first
```

## Summary

**TPPs are your institutional memory** when context windows fill up.

**Key habits:**

1. Create TPP for complex work
2. Use `/tpp` to execute phases
3. Update as you learn
4. Use `/handoff` when context fills
5. Archive to `_done/` when complete

**Think of TPPs as notes to your future self** - what would you want to know when returning to this work in 2 weeks?

---

**Further Reading:**

- Original concept: https://photostructure.com/coding/claude-code-tpp/
- CLAUDE.md - Project-specific conventions
- TDD.md - Testing methodology
