# PICOCluster Design Document

> Living design document — updated as the spec evolves.

---

## 1. Overview

PICOCluster is a distributed computing system composed of Raspberry Pi PICO W microcontrollers. Each node runs a shared **PCode virtual machine** (bytecode interpreter) capable of hosting multiple cooperative threads. Nodes communicate exclusively over WiFi; together they form a cluster that can spread thread execution across available hardware automatically.

---

## 2. Goals

- Run cooperative multithreaded PCode programs on each PICO W node.
- Distribute thread execution across nodes based on real-time load and memory availability.
- Communicate entirely over WiFi using raw TCP/IP or UDP — no external file-sharing protocols (NFS, SMB, etc.).
- Provide locking primitives so threads running on different nodes can safely share state.
- Model shared memory as network-accessible "files" hosted on a central Controller Node.
- Provide a Controller Node that monitors cluster health, exposes a file API, and can dispatch PCode programs to any PICO.

---

## 3. PCode Virtual Machine

Each PICO W runs an interpreter that fetches, decodes, and executes PCode bytecodes.

### 3.1 Cooperative Multithreading

Two conditions trigger a thread switch:

1. **Time-slice expiry** — the running thread has consumed its 2 ms time slice.
2. **Resource block** — the running thread executes an instruction that must wait for a resource (I/O, lock, network); it yields immediately rather than stalling the node.

#### Time-slice mechanism

The time slice is **2 milliseconds**. Rather than a hardware interrupt, the check is performed by the fetch-decode-execute loop itself — before dispatching each instruction the interpreter checks whether the current thread's slice has expired:

```
loop:
    if (now() - thread.start_time) >= 2ms:
        switch_thread()          // save context, pick next thread
        thread.start_time = now() // reset slice for resumed thread
    fetch → decode → execute next instruction
    goto loop
```

The thread's start time is stored **inside the thread data structure** (see §3.1.1). It is written once when the thread first begins execution and again every time the thread is resumed after a switch. This makes the slice self-contained per thread — no shared timer state is needed.

#### 3.1.1 Thread data structure (partial)

The thread context block holds everything needed to suspend and resume a thread. Fields defined so far:

| Field | Size | Description |
|---|---|---|
| `start_time` | 4 bytes | Timestamp (ms) when this thread last began or resumed execution; compared against `now()` to detect slice expiry |
| `R0`–`R7` | 32 bytes | Saved general-purpose register values |
| `IP` | 4 bytes | Instruction pointer — address of the next instruction to execute |
| `SP` | 4 bytes | Stack pointer |
| *(future fields)* | — | Thread state flags, priority, migration info, etc. |

A context switch saves the running thread's registers and `IP`/`SP` into its thread data block, selects the next runnable thread, restores its registers and `IP`/`SP`, and sets its `start_time` to the current time.

### 3.2 Instruction Encoding

Each instruction is laid out in memory as:

```
[ OPCODE (1 byte) ] [ ADDR_MODE ... (0–2 bytes) ] [ OPERAND ... (0–20 bytes) ]
```

#### 3.2.1 Opcode byte

One byte → 256 distinct instructions. The main fetch-decode-execute loop reads a single byte and dispatches immediately to the handler for that instruction. Because the handler knows the instruction's operand count, it then pulls exactly the right number of addressing-mode and operand bytes. This keeps the dispatch path minimal and fast.

#### 3.2.2 Addressing mode byte(s)

Each addressing mode byte encodes two operand modes, one per nibble (4 bits each → **16 addressing modes** per operand slot):

```
  Bits 7–4 : addressing mode for operand N
  Bits 3–0 : addressing mode for operand N+1  (zero if operand N+1 does not exist)
```

| Operand count | Addressing mode bytes |
|:---:|:---:|
| 0 | 0 |
| 1 | 1 (high nibble = mode, low nibble = 0x0) |
| 2 | 1 (high nibble = mode₁, low nibble = mode₂) |
| 3 | 2 (byte 0: modes 1 & 2; byte 1: high nibble = mode₃, low nibble = 0x0) |
| 4 | 2 (byte 0: modes 1 & 2; byte 1: modes 3 & 4) |

#### 3.2.3 Data sizes

All integer values and addresses are **32 bits (4 bytes)**. This matches the PICO W's native word size and keeps the memory footprint small. The encoding is designed so the integer width can be increased later (e.g., to 64-bit) by redefining the addressing mode operand sizes — the instruction format itself does not need to change.

#### 3.2.4 Addressing modes

Five modes are defined, occupying nibble values `0x0`–`0x4`. Nibble values `0x5`–`0xF` are reserved for future use.

| Nibble | Mnemonic | Operand bytes | Description |
|:---:|:---:|:---:|---|
| `0x0` | `IMM` | 4 | **Immediate** — 32-bit value encoded inline in the instruction stream |
| `0x1` | `REG` | 1 | **Register** — register index (0–7); value is the register's contents |
| `0x2` | `IND` | 1 | **Indirect** — register index (0–7); the register holds a 32-bit address, and the operand value is read from that address |
| `0x3` | `IDX` | 5 | **Indexed** — base register index (0–7, 1 byte) + 4-byte signed 32-bit displacement; effective address = register value + displacement |
| `0x4` | `DIR` | 4 | **Direct** — 32-bit absolute address encoded inline; the operand value is read from that address |
| `0x5`–`0xF` | *(reserved)* | — | Available for future modes (e.g., PC-relative, stack-relative) |

#### 3.2.5 Registers

The PCode VM has three categories of registers. All are 32-bit and all are **per-thread** — each thread carries its own complete register state, so a cooperative context switch is simply a swap of this set.

| Register | Type | Description |
|---|---|---|
| `R0`–`R7` | General-purpose (data) | Eight 32-bit data registers for arithmetic, addressing, and temporary values |
| `IP` | Instruction Pointer | Address of the next instruction to fetch; advanced automatically by the fetch loop and modified by `JUMP`, `CALL`, and `RETURN` |
| `SP` | Stack Pointer | Address of the top of the current thread's stack; updated by `CALL`, `RETURN`, and any explicit push/pop instructions |

The register index field in `REG`, `IND`, and `IDX` operands addresses only the data registers; only values `0`–`7` are valid — the upper 5 bits of the index byte are unused and should be zero. `IP` and `SP` are not directly addressable as general operands; they are manipulated implicitly by the instructions that own them.

#### 3.2.7 Operands

Operand bytes follow the addressing mode byte(s) in left-to-right operand order. The number of bytes consumed per operand is determined entirely by its addressing mode (see table above).

Worst-case instruction length — four `IDX` operands:

```
1 (opcode) + 2 (addr mode bytes) + 4 × 5 (IDX operands) = 23 bytes
```

Instruction size range: **1 byte** (opcode only, no operands) to **23 bytes** (4 × indexed operands).

#### 3.2.8 Instruction set (preliminary)

| Category       | Instructions                                      |
|----------------|---------------------------------------------------|
| Arithmetic     | `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `SQRT`        |
| Control flow   | `CALL`, `RETURN`, `JUMP`, conditional jumps       |
| Threading      | `START_THREAD`, `END_THREAD`                      |
| I/O            | `PRINT`, `MESSAGE`                                |
| Memory/sync    | TBD — load/store ops, lock/unlock primitives      |
| Network/file   | TBD — read/send operations over TCP/IP or UDP     |

---

## 4. Network Architecture

### 4.1 WiFi Configuration

Each PICO W stores **all of its configuration in a single local file** on its flash. This includes WiFi credentials (SSID, PASSWORD) and every tunable cluster parameter: heartbeat interval, full-load thresholds (CPU and memory), neighbor table size, residency timeout, and any future settings. Consolidating everything into one file has two benefits:

1. A node only needs to read one place at boot to be fully configured.
2. The Controller can push an updated config file to any or all nodes using the file-distribution message (see §4.3.4), triggering a rolling configuration upgrade on the next reboot of each node.

The config file uses a **flat key=value format** (INI-style, but with no sections). Example:

```
SSID=mywifi
PASSWORD=secret
HEARTBEAT_INTERVAL_MS=500
FULL_LOAD_CPU_PCT=80
FULL_LOAD_MEM_PCT=80
NEIGHBOR_TABLE_SIZE=8
```

No section headers (`[section]`) are used — the file is a plain list of `key=value` pairs, one per line. Comments may use `#`. This keeps the format simple in both target implementations:

- **MicroPython**: the file can be structured as a plain Python module (`KEY = value`) and imported directly — no parser needed at all.
- **C (Pico SDK)**: a stateless line-by-line parser suffices; no section state needs to be tracked. Each line is split on `=`, whitespace trimmed, and the key looked up in a fixed table.

### 4.2 Node Heartbeat / Discovery

Each node continuously **listens** for state-broadcast messages from its peers and **broadcasts** its own state at a high frequency so that new or recovering nodes can populate their neighbor tables quickly.

#### Broadcast message fields (extensible)

| Field | Description |
|---|---|
| **Version** | PCode VM version — a single unsigned integer; the Controller admits the node only if this matches the expected version exactly |
| Node identifier | Unique ID for the sending node |
| IP address | Sender's current IP on the WiFi network |
| Current CPU load | Used for thread-migration decisions |
| Free memory | Used for thread-migration decisions |
| *(future fields)* | Additional state as the spec grows |

Messages are sent over UDP broadcast so no prior knowledge of peer addresses is required.

#### Neighbor table

Each PICO node maintains a **bounded neighbor table** — a fixed-size list of peers it currently considers reachable and usable for thread migration. The table size defaults to **8 entries** and is configurable.

The table represents *usable* neighbors, not merely known ones. Entries are evicted by the following rules, evaluated in priority order when a slot is needed or on each heartbeat tick:

| Priority | Condition | Action |
|---|---|---|
| 1 (highest) | No heartbeat received for **3 × broadcast interval** | Evict immediately — node is considered dead or stuck |
| 2 | Node has been in the table for longer than a configured **residency timeout** AND its load is at or above the **full-load threshold** | Evict — node is persistently saturated and unlikely to accept work; free the slot for a more useful neighbor |
| 3 | Table is full and a new node announces itself | Evict the entry that best matches priority 2 criteria, or failing that the least-recently-updated entry |

Key points:
- **Dead node recovery**: The 3× timeout ensures the table self-heals when a node crashes or freezes, without any central coordinator.
- **Load-based rotation**: A persistently saturated neighbor is only useful as a fallback; evicting it lets a fresher, lighter node into the table and keeps the neighbor list actionable for thread dispatch.
- **Configurable thresholds**: The residency timeout and full-load threshold are tunable parameters, not hard-coded constants. Initial defaults: **CPU utilization ≥ 80%** OR **memory utilization ≥ 80%** (either condition alone is sufficient to flag a node as fully loaded). The memory threshold in particular is expected to be revisited once typical PCode program sizes are known.

#### Neighbor table — Controller Node behavior

The Controller Node is **not subject to these eviction rules**. It:
- Tracks every node it has ever seen, without a size cap.
- Retains the last known state of any node that goes silent (marked **stale** but not deleted).
- Stale entries are only purged by explicit user action from the dashboard.

This gives the Controller a complete historical picture of the cluster — including nodes that crashed or were removed — while each PICO's neighbor table stays lean and contains only nodes worth dispatching work to.

#### Broadcast interval

Broadcasts are sent every **500 milliseconds**. This interval balances two requirements:
- A new node joining the network hears from all active peers within a few seconds.
- The thread-migration subsystem always has a fresh view of cluster load.

The dead-node eviction timeout (3× interval) is therefore **1.5 seconds**.

### 4.3 Controller Node

The **Controller Node** is a dedicated non-PICO host (e.g., a Raspberry Pi or desktop machine) that serves as the administrative hub of the cluster. It is not resource-constrained and runs conventional software. Its responsibilities span four areas:

#### 4.3.1 File Storage & API

The Controller hosts all cluster-wide shared files and exposes a simple TCP-based file API to the PICO nodes. File operations are raw TCP message exchanges — no NFS or SMB.

| Storage tier | Location | Access method |
|---|---|---|
| Cluster-wide / shared files | Controller Node filesystem | TCP file API (see below) |
| Node-local files | Each PICO's own flash | Local filesystem |

Minimum file API operations (exact wire format TBD):

| Operation | Description |
|---|---|
| `OPEN` | Request access to a named file; returns a handle |
| `READ` | Read bytes from an open handle |
| `WRITE` | Write bytes to an open handle |
| `CLOSE` | Release the handle |
| `LIST` | Enumerate available shared files |

"File" means any named blob of data — **not** a mounted filesystem or NFS/SMB share.

#### 4.3.2 Node Bootstrap Protocol

Neither the PICO nor the Controller needs any pre-configured knowledge of the other's address. Discovery is fully automatic:

```
PICO                                         Controller
 |                                                 |
 |-- UDP broadcast: heartbeat (version, ID, ...) ->|
 |                                                 |
 |          [Controller checks version]            |
 |                                                 |
 |  version OK:                                    |
 |<-- UDP unicast: CONTROLLER_ANNOUNCE ------------|
 |    (Controller IP, file API port, ...)          |
 |                                                 |
 |-- TCP connect to Controller ----------------->  |
 |                                                 |
 |<======== file API, program dispatch ===========>|
 |                                                 |
 |  version MISMATCH:                              |
 |    (no reply — PICO stays isolated)             |
 |    Controller logs PICO as "needs update"       |
```

Step by step:

1. **PICO boot** — after joining WiFi the PICO immediately begins broadcasting heartbeats (§4.2). It has no knowledge of the Controller's address.
2. **Controller hears heartbeat and checks version** — the Controller reads the version field from the heartbeat and compares it to the current expected version.
   - **Version matches** — the Controller sends a `CONTROLLER_ANNOUNCE` message as a **UDP unicast** directly to the PICO's IP (taken from the heartbeat). The announce message contains the Controller's IP and TCP port for the file API.
   - **Version mismatch** — the Controller does **not** send `CONTROLLER_ANNOUNCE`. The PICO receives no reply and remains isolated from the cluster. The Controller records the node in a separate **quarantine list** (node ID, IP, reported version) for later remediation.
3. **PICO receives announce** — the PICO stores the Controller's address and opens a **persistent TCP connection** to the Controller's file API port. All subsequent file operations (OPEN, READ, WRITE, CLOSE, LIST) and program dispatch travel over this connection.
4. **Reconnection** — if the TCP connection drops (Controller restart, network blip), the PICO's ongoing heartbeats will trigger a fresh version check and `CONTROLLER_ANNOUNCE` from the Controller as soon as it is back, causing the PICO to reconnect automatically.

Key properties of this design:
- **Zero pre-configuration** on either side — no static addresses, no config files listing peers.
- **Version-gated admission** — only PICOs running the expected firmware version join the active cluster; older or mismatched nodes are visible to the operator but cannot participate.
- **Controller-initiated connection** — the Controller reaches out first (unicast), so the PICO never needs to know where to look.
- **PICO-initiated TCP** — the PICO opens the durable connection, so the Controller does not need to manage inbound TCP sockets per PICO at startup.
- The `CONTROLLER_ANNOUNCE` message is **not** a heartbeat and does not enter any PICO's neighbor table.

#### 4.3.3 Cluster Monitoring

The Controller listens to the same UDP heartbeat broadcasts that the PICO nodes exchange (§4.2). It maintains two separate node registries:

**Active nodes** — PICOs whose heartbeat version matches the current expected version and that have successfully connected via TCP:
- Per-node: IP address, CPU load, free memory, running thread count
- History of node state over time (for graphs and alerting)

**Quarantine list** — PICOs heard on the network but running an incompatible version:
- Per-node: IP address, reported version number, time first seen, time last seen
- These nodes are **not** invited to connect and cannot access shared files or receive dispatched programs
- Entries are never automatically purged — they persist until the operator clears them or the node reappears with the correct version (at which point it moves to the active list)

Unlike PICO nodes, the Controller is not bounded by an 8-entry neighbor table — it tracks every node it can hear, regardless of version.

#### 4.3.4 Controller→PICO Message System

The existing TCP connection between the Controller and each PICO (established during bootstrap, §4.3.2) is a **bidirectional message channel**. The Controller sends typed messages to PICO nodes; each message requires an acknowledgement response. This is the mechanism for both program dispatch and file distribution.

##### Defined message types

| Message | Direction | Description |
|---|---|---|
| `EXEC_PROGRAM` | Controller → PICO | Deliver a PCode binary and begin executing it immediately |
| `STORE_FILE` | Controller → PICO | Write a named file to the PICO's local flash storage |

Additional message types are expected as the design matures.

##### Acknowledgement semantics

Every Controller→PICO message follows a two-phase acknowledgement:

| Response | Meaning |
|---|---|
| `LOADING` | PICO has received the message and has begun processing (receiving the payload, writing to flash, etc.) |
| `DONE` | Processing completed successfully |
| `ERROR` | Processing failed; payload contains a short reason string |

The Controller tracks outstanding acknowledgements per node and surfaces the status in the dashboard (see §4.3.5).

##### Program dispatch flow

1. User selects a PCode binary and a target node (or "any available node") in the dashboard.
2. Controller sends `EXEC_PROGRAM` to the target node over TCP.
3. PICO replies `LOADING`, then `DONE` once the program is running.
4. The Controller monitors ongoing health via subsequent heartbeats.

##### File distribution and rolling configuration upgrade

The `STORE_FILE` message allows the Controller to push any file — including the node's config file — to a PICO's local flash:

1. Controller sends `STORE_FILE` with the target filename and content.
2. PICO replies `LOADING`, writes the file to local flash, then replies `DONE`.
3. The new file takes effect on the **next reboot** of that node.

When the Controller distributes an updated config file to all active nodes, each node will pick up the new configuration as it naturally reboots (or is rebooted by the operator). Because nodes reboot independently, this constitutes a **rolling configuration upgrade** with no coordinated cluster downtime.

#### 4.3.5 Dashboard GUI

The Controller runs a graphical dashboard providing a real-time view of the cluster. Intended panels:

| Panel | Description |
|---|---|
| **Active nodes** | One tile per connected PICO — node ID, IP, status indicator |
| **CPU utilization** | Rolling graph of CPU load per active node (similar to a system monitor) |
| **Memory** | Free/used memory per active node |
| **Shared files** | List of files currently hosted on the Controller; open handles |
| **Shared memory** | Active shared-memory regions and which nodes are accessing them |
| **Dispatch** | UI to send messages to one or all active nodes: execute a PCode program (`EXEC_PROGRAM`) or push a file to local flash (`STORE_FILE`); shows per-node acknowledgement status (`LOADING` / `DONE` / `ERROR`) |
| **Quarantine list** | PICOs running an incompatible version — node ID, IP, reported version, last seen; operator can initiate a firmware update from here |

The dashboard is a read/write interface — it can observe the cluster and also act on it (dispatch programs, inspect or modify shared files).

---

## 5. Distributed Execution

### 5.1 Thread Migration

- A thread can execute on **any node** in the cluster.
- The runtime monitors heartbeat data and identifies underutilized nodes.
- When migration is warranted, the originating node serializes the thread's code and data and hands it off to the target PICO W.

### 5.2 Thread Safety & Locking

- Threads share state via network-accessible "files" (see §4.3).
- A **locking mechanism** protects shared resources from concurrent access by threads on different nodes.
- The exact locking scheme (centralized lock server vs. distributed protocol) is TBD — see [Open Questions](#6-open-questions).

### 5.3 Shared Memory Model

Shared memory is modeled as named "files" stored on the central host. Any node reads or writes these files over TCP/IP or UDP, guarded by the locking primitives above.

---

## 6. Open Questions

These items need resolution before or during initial implementation:

1. **Implementation language** — MicroPython (easier iteration) vs. C with the Pico SDK (lower overhead, faster interpreter)?
2. ~~**Bytecode encoding**~~ — **resolved**: 1-byte opcode, 0–2 addressing-mode bytes (4 bits per operand), 0–4 operands; integers and addresses are 32-bit; 1–23 bytes per instruction (see §3.2).
   - **Remaining sub-question**: endianness of multi-byte operands (little-endian preferred for PICO W / ARM Cortex-M33, but not yet locked in).
3. ~~**Node discovery protocol**~~ — **resolved**: UDP broadcast heartbeats; each node listens and maintains a bounded neighbor table (see §4.2).
4. ~~**Neighbor table size limit**~~ — **resolved**: default **8 entries**, configurable (see §4.2).
5. ~~**Heartbeat broadcast interval**~~ — **resolved**: **500 ms**; dead-node eviction timeout is 3× = **1.5 s** (see §4.2).
6. ~~**Full-load threshold**~~ — **resolved**: **CPU utilization ≥ 80%** OR **memory utilization ≥ 80%** (either alone is sufficient); both are tunable defaults. Memory threshold expected to be revisited once typical PCode program sizes are known (see §4.2).
7. **Residency timeout** — how long must a node be in the table while fully loaded before it becomes a candidate for load-based eviction?
8. ~~**Addressing mode table**~~ — **resolved**: IMM (0x0, 4 bytes), REG (0x1, 1 byte), IND (0x2, 1 byte), IDX (0x3, 5 bytes), DIR (0x4, 4 bytes); 0x5–0xF reserved (see §3.2.4).
   - ~~Remaining sub-question: number of registers~~ — **resolved**: 8 registers (`R0`–`R7`), index range 0–7 (see §3.2.5).
9. **Lock implementation** — centralized lock server on the Controller Node? Token-ring? Distributed algorithm (e.g., Lamport)?
10. ~~**Periodic switch interval**~~ — **resolved**: 2 ms time slice; checked before each instruction using `start_time` stored in thread data; reset on every resume (see §3.1).
11. ~~**Version number format**~~ — **resolved**: single unsigned integer, exact match required (see §4.2).
12. ~~**Controller Node identity**~~ — **resolved**: zero pre-configuration on both sides; Controller hears PICO heartbeats, checks version, and replies with `CONTROLLER_ANNOUNCE` only to matching nodes; PICO then opens a persistent TCP connection (see §4.3.2).
    - **Remaining sub-question**: `CONTROLLER_ANNOUNCE` wire format — what fields beyond IP and file API port?
13. **File API wire format** — message framing, encoding (binary vs. text), and error responses for OPEN/READ/WRITE/CLOSE/LIST?
14. **Dashboard technology** — what UI framework/language for the Controller dashboard (web app, native GUI, terminal UI)?
15. ~~**Program dispatch acknowledgement**~~ — **resolved**: the dispatch mechanism is a general Controller→PICO message system over the existing TCP connection; every message receives a two-phase `LOADING` → `DONE` (or `ERROR`) acknowledgement (see §4.3.4). Wire format of messages and acknowledgements is TBD (see #13).
16. ~~**Config file format**~~ — **resolved**: flat `key=value` format (INI-style, no sections); `#` for comments. In MicroPython the file can double as an importable module; in C a stateless line-by-line parser suffices (see §4.1).
