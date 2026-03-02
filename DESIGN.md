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

- The interpreter performs a **periodic thread switch** at a configurable interval (e.g., every N instructions).
- Any instruction that blocks waiting for a resource (I/O, lock, network) triggers an **immediate thread switch** rather than stalling the node.

### 3.2 Instruction Encoding

Each instruction is laid out in memory as:

```
[ OPCODE (1 byte) ] [ ADDR_MODE ... (1 or 2 bytes) ] [ OPERAND ... (0–32 bytes) ]
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

#### 3.2.3 Operands

Operand bytes follow the addressing mode byte(s) in left-to-right operand order. The addressing mode for each operand determines how many bytes it consumes. The maximum operand size is **8 bytes** (64-bit value), giving a worst-case instruction length of:

```
1 (opcode) + 2 (addr mode) + 4 × 8 (operands) = 35 bytes
```

Instruction size range: **1 byte** (opcode only, no operands) to **35 bytes** (4 × 8-byte operands).

#### 3.2.4 Addressing modes (TBD)

The 16 mode values per nibble are not yet assigned. Candidates include immediate, register, direct memory, indirect, indexed, and others. See [Open Questions](#6-open-questions).

#### 3.2.5 Instruction set (preliminary)

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

Each PICO W stores its WiFi credentials (SSID and PASSWORD) in a local configuration file. At boot the node connects to the shared WiFi network.

### 4.2 Node Heartbeat / Discovery

Each node continuously **listens** for state-broadcast messages from its peers and **broadcasts** its own state at a high frequency so that new or recovering nodes can populate their neighbor tables quickly.

#### Broadcast message fields (extensible)

| Field | Description |
|---|---|
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
- **Configurable thresholds**: The residency timeout and full-load threshold (e.g., CPU ≥ 90%, or free memory below a floor) are tunable parameters, not hard-coded constants.

#### Neighbor table — Controller Node behavior

The Controller Node is **not subject to these eviction rules**. It:
- Tracks every node it has ever seen, without a size cap.
- Retains the last known state of any node that goes silent (marked **stale** but not deleted).
- Stale entries are only purged by explicit user action from the dashboard.

This gives the Controller a complete historical picture of the cluster — including nodes that crashed or were removed — while each PICO's neighbor table stays lean and contains only nodes worth dispatching work to.

#### Broadcast interval

Broadcasts are sent at a **short, fixed interval** (exact value TBD) so that:
- A new node joining the network hears from all active peers within a few seconds.
- The thread-migration subsystem always has a fresh view of cluster load.

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

#### 4.3.2 Cluster Monitoring

The Controller listens to the same UDP heartbeat broadcasts that the PICO nodes exchange (§4.2). It maintains a view of the entire cluster:

- Which nodes are currently visible (reachable on the network)
- Per-node: IP address, CPU load, free memory, running thread count
- History of node state over time (for graphs and alerting)

Unlike PICO nodes, the Controller is not bounded by an 8-entry neighbor table — it tracks all nodes it can see.

#### 4.3.3 Program Dispatch

The Controller can **send PCode programs to any node** for execution. This provides a single point of control for launching workloads across the cluster without needing direct access to individual PICOs.

Dispatch flow (details TBD):
1. User selects a program and a target node (or "any available node") in the dashboard.
2. Controller transmits the PCode binary to the target node over TCP.
3. Target node loads the program and begins execution; the Controller monitors progress via subsequent heartbeats.

#### 4.3.4 Dashboard GUI

The Controller runs a graphical dashboard providing a real-time view of the cluster. Intended panels:

| Panel | Description |
|---|---|
| **Node list** | One tile per visible PICO — node ID, IP, status indicator |
| **CPU utilization** | Rolling graph of CPU load per node (similar to a system monitor) |
| **Memory** | Free/used memory per node |
| **Shared files** | List of files currently hosted on the Controller; open handles |
| **Shared memory** | Active shared-memory regions and which nodes are accessing them |
| **Program dispatch** | UI to select a PCode binary and send it to one or all nodes |

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
2. ~~**Bytecode encoding**~~ — **resolved**: 1-byte opcode, 0–2 addressing-mode bytes (4 bits per operand), 0–4 operands up to 8 bytes each; 1–35 bytes per instruction (see §3.2).
   - **Remaining sub-question**: endianness of multi-byte operands (little-endian preferred for PICO / ARM, but not yet locked in).
   - **Remaining sub-question**: the 16 addressing mode encodings — see item below.
3. ~~**Node discovery protocol**~~ — **resolved**: UDP broadcast heartbeats; each node listens and maintains a bounded neighbor table (see §4.2).
4. ~~**Neighbor table size limit**~~ — **resolved**: default **8 entries**, configurable (see §4.2).
5. **Heartbeat broadcast interval** — exact interval in milliseconds? Tradeoff between discovery latency and WiFi traffic.
   - Dead-node timeout is derived from this value (3× interval), so the interval also controls how quickly a crashed node is evicted.
6. **Full-load threshold** — what CPU load % and/or free memory floor defines "fully loaded" for eviction purposes?
7. **Residency timeout** — how long must a node be in the table while fully loaded before it becomes a candidate for load-based eviction?
8. **Addressing mode table** — define all 16 mode encodings (e.g., immediate, register, direct, indirect, indexed, …) and the operand byte size each implies.
10. **Lock implementation** — centralized lock server on the Controller Node? Token-ring? Distributed algorithm (e.g., Lamport)?
11. **Periodic switch interval** — instruction count threshold for cooperative yield?
12. **Controller Node identity** — is the Controller's address static/pre-configured on each PICO, or announced via a special heartbeat field?
13. **File API wire format** — message framing, encoding (binary vs. text), and error responses for OPEN/READ/WRITE/CLOSE/LIST?
14. **Dashboard technology** — what UI framework/language for the Controller dashboard (web app, native GUI, terminal UI)?
15. **Program dispatch acknowledgement** — how does the Controller know a dispatched program was received and started successfully?
