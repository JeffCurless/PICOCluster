# PICOCluster — Claude Code Instructions

## Project status
Greenfield project — design phase only. No code exists yet.

## Living design document
`DESIGN.md` is the authoritative design document. Keep it up to date whenever a decision is made, a question is resolved, or the spec evolves. Do not let decisions made in conversation go unrecorded.

### DESIGN.md conventions
- Resolved open questions are **struck through** with a `~~text~~` resolved note — never deleted. This preserves the decision history.
- Open questions that are partially resolved get sub-bullet notes under them.
- New open questions are appended to §6 and numbered sequentially.
- Subsections are numbered (e.g. 3.2.1, 4.3.2) — renumber downstream sections when inserting new ones.

## Key decisions already made (do not re-open without user instruction)
- **PCode VM**: 1-byte opcode, 0–2 addressing-mode bytes (4 bits per operand), 32-bit integers and addresses, 1–23 bytes per instruction.
- **Addressing modes**: IMM (0x0), REG (0x1), IND (0x2), IDX (0x3), DIR (0x4); 0x5–0xF reserved.
- **Registers**: R0–R7 (data), IP, SP — all per-thread, all 32-bit.
- **Thread switch**: 2 ms time slice, checked before each instruction using `start_time` stored in thread data; also triggers on resource block.
- **Neighbor table**: 8 entries (configurable); eviction by dead-node timeout (3× heartbeat interval), then load-based rotation, then LRU.
- **Heartbeat**: UDP broadcast; fields include version (single unsigned integer, exact match required), node ID, IP, CPU load, free memory.
- **Controller discovery**: zero pre-configuration; Controller hears heartbeats, checks version, sends `CONTROLLER_ANNOUNCE` unicast to matching nodes; PICO opens persistent TCP connection.
- **Version mismatch**: non-matching PICOs are quarantined (tracked, not connected); quarantine list visible in dashboard.
- **File API**: OPEN, READ, WRITE, CLOSE, LIST over TCP — no NFS/SMB.

## Implementation language
Not yet decided — MicroPython vs. C with Pico SDK is open question #1. Do not assume either until the user decides.

## General preferences
- Update DESIGN.md in the same response as the conversation decision — do not wait to be asked.
- Keep changes focused and minimal — do not refactor or expand sections beyond what was discussed.
- Do not auto-commit.
