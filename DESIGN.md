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

### 3.2 Instruction Set (preliminary)

| Category       | Instructions                                      |
|----------------|---------------------------------------------------|
| Arithmetic     | `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `SQRT`        |
| Control flow   | `CALL`, `RETURN`, `JUMP`, conditional jumps       |
| Threading      | `START_THREAD`, `END_THREAD`                      |
| I/O            | `PRINT`, `MESSAGE`                                |
| Memory/sync    | TBD — load/store ops, lock/unlock primitives      |
| Network/file   | TBD — read/send operations over TCP/IP or UDP     |

The binary encoding (opcode width, operand layout, calling convention) is not yet defined — see [Open Questions](#6-open-questions).

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

Each node maintains a **bounded neighbor table** — a fixed-size list of the most recently heard-from peers. When the table is full and a new node announces itself, the oldest (or least recently heard) entry is evicted. This cap limits cluster complexity and memory use on constrained hardware.

The neighbor table size defaults to **8 entries** and is configurable. This fits comfortably in PICO W RAM and sets the practical maximum number of peers any single node actively tracks.

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
2. **Bytecode encoding** — opcode byte width, operand format, endianness, calling convention?
3. ~~**Node discovery protocol**~~ — **resolved**: UDP broadcast heartbeats; each node listens and maintains a bounded neighbor table (see §4.2).
4. ~~**Neighbor table size limit**~~ — **resolved**: default **8 entries**, configurable (see §4.2).
5. **Heartbeat broadcast interval** — exact interval in milliseconds? Tradeoff between discovery latency and WiFi traffic.
6. **Thread migration protocol** — serialization format for code segment, operand stack, and local variables during handoff?
7. **Lock implementation** — centralized lock server on the Controller Node? Token-ring? Distributed algorithm (e.g., Lamport)?
8. **Periodic switch interval** — instruction count threshold for cooperative yield?
9. **Controller Node identity** — is the Controller's address static/pre-configured on each PICO, or announced via a special heartbeat field?
10. **File API wire format** — message framing, encoding (binary vs. text), and error responses for OPEN/READ/WRITE/CLOSE/LIST?
11. **Dashboard technology** — what UI framework/language for the Controller dashboard (web app, native GUI, terminal UI)?
12. **Program dispatch acknowledgement** — how does the Controller know a dispatched program was received and started successfully?
