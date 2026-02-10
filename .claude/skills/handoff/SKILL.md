---
name: handoff
description: Update TPP with session discoveries, completed work, and blockers. Use when context is getting full (80-90%) or when switching tasks.
disable-model-invocation: true
allowed-tools: Read, Edit, Grep, Glob
---

# TPP Handoff Workflow

Update a Technical Project Plan with everything learned in this session so the next engineer (or AI session) can continue seamlessly.

## When to Use This

- Context window is 80-90% full
- Switching to a different task
- About to lose session context
- Major discovery that changes the plan
- Completed a phase of work

## What to Document

### 1. Phase Progress

Update phase checkboxes:
- Mark completed phases with `[x]`
- Keep current/next phase unchecked `[ ]`

### 2. Tribal Knowledge

Add to the "Tribal Knowledge" section:
- **Non-obvious discoveries**: Things that aren't in docs
- **Gotchas**: Edge cases, pitfalls, surprising behavior
- **Why not X**: Approaches tried and why they didn't work
- **Dependencies**: What depends on what
- **Historical context**: Why things are the way they are

Example:
```markdown
## Tribal Knowledge

- SQLite's vector extension must be loaded before creating virtual tables
- The `ann_search()` function requires normalized vectors (discovered through segfault debugging)
- Attempted brute-force approach first, but performance was O(n²) - switched to ANN algorithm
- Distance calculations use squared Euclidean for performance (avoids sqrt)
```

### 3. Tasks Status

Update the "Tasks" section:
- Mark completed tasks with `[x]`
- Add verification commands you used
- Document test results
- Add newly discovered tasks

### 4. Solutions Evaluation

Update the "Solutions" section:
- Add pros/cons discovered during implementation
- Document why chosen approach is working (or isn't)
- Note alternative approaches to try if current fails

### 5. Clear Blockers

If blocked, be explicit:
```markdown
## BLOCKER

Cannot proceed until:
- [ ] User provides API key for embeddings service
- [ ] Upstream bug in SQLite extension is fixed

Next session should start by addressing these blockers.
```

## Handoff Checklist

Before ending session, ensure TPP includes:
- [ ] Current phase is clear (first unchecked checkbox)
- [ ] All discoveries documented in Tribal Knowledge
- [ ] Failed approaches documented with reasons
- [ ] Task status updated
- [ ] Any blockers clearly stated
- [ ] Next steps are obvious to next session

## Example Invocation

```
/handoff 20250209-ann-search
```

Prompts you to update `_todo/20250209-ann-search.md` with session discoveries.

## Format

The handoff should read like **notes from a departing engineer**:
- What did I learn?
- What worked? What didn't?
- What should the next person know?
- Where should they start?

Don't just list what you did - explain the **why** behind decisions and discoveries.

## Workflow

1. Read the TPP: `_todo/$ARGUMENTS.md`
2. Review conversation history for key discoveries
3. Update relevant sections of the TPP
4. Verify handoff checklist is complete
5. Confirm with user that handoff is ready

## Example Handoff Entry

```markdown
## Tribal Knowledge (Updated 2025-02-09)

**Session findings:**
- Vector normalization is critical - SQLite's vector extension segfaults on non-normalized vectors
- Tried HNSW algorithm first, but memory usage was prohibitive for large datasets (>1M vectors)
- Switched to IVF-based approach with 256 clusters - 10x faster queries
- Distance threshold of 0.15 gives good precision/recall tradeoff for our use case
- Test coverage gap: need integration tests for concurrent searches

**Blockers:**
- Need user decision: should we support cosine similarity or only Euclidean distance?
- SQLite extension build requires gcc 11+ (not available in CI yet)

**Next session should:**
1. Resolve blocker about similarity metric
2. Add integration tests for concurrent searches
3. Continue with "Integration" phase
```
