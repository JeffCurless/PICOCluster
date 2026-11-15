# picoCluster

A cluster environment for Raspberry Pi Pico W boards, built as a layer on
[picoOS](https://github.com/JeffCurless/picoOS) (a git submodule, used
unmodified).

A main application starts on one node and launches "threads" on as many nodes as
it needs. Every node runs the same firmware. A thread is a **cluster task**: a
named function in `apps/cluster_tasks.c`. The creating node sends the task name
plus argument bytes. The node that runs it executes the function in its own
picoOS process and sends the result bytes back.

```c
cluster_thread_t t;
cluster_thread_create(&t, "pi", &args, sizeof(args), CLUSTER_NODE_ANY);
...
cluster_thread_join(&t, &result, sizeof(result), &len, &status, 60000u);
```

See `cluster/cluster.h` for the API and `apps/cpi.c` for a complete example.

## Layout

| Path | What |
|---|---|
| `cluster/` | The cluster layer: protocol, transport, node table, workers, daemon |
| `apps/` | `app_table.c` (shell apps), `cluster_tasks.c` (cluster tasks), `cpi.c` demo |
| `tests/` | Host-native unit tests |
| `picoOS/` | The operating system (submodule; do not edit here) |
| `builds/`, `kits/` | Build tree and finished `.uf2` images (contents not tracked) |

## Build

```bash
git submodule update --init
./build            # Pico W + Display Pack and Display Pack 2 -> kits/
./build D          # Display Pack only   (./build D2 for Display Pack 2)
./build tests      # host unit tests
./clean            # empty builds/, remove tests/build and kits/*.uf2
```

`PICO_SDK_PATH` selects the SDK. The default is `~/workspace/pico-sdk`.

## Setting up a node

Flash the same `.uf2` to every board. Then give each board a `config.txt` on its
picoOS filesystem:

```
AUTORUN=clusterd
NODEID=0
SSID=mynetwork
PASSWORD=secret
```

- `NODEID` must be unique in the cluster (0..65534).
- `SSIDALT=` and `PASSWORDALT=` set an optional fallback network.
- Keep `AUTORUN=` within the first 256 bytes of the file. picoOS only reads
  that far.

## Using it

From the picoOS shell on any node:

```
cluster              status: node id, IP, build id, free workers
cluster nodes        every node heard from, with free worker slots
cluster jobs         requests from this node, and tasks running on it
cluster run echo hi  run a task anywhere and print its result
cluster run echo hi 1    ...on node 1
run cpi 12           distributed Monte Carlo pi with 12 threads
```

### Display Pack status page

While clusterd runs, the panel shows:

- **Title bar:** the node id and the picoCluster version.
- **Network line:** the IP and node count. Before clusterd is up, this shows
  the startup or error state instead: joining WiFi, config missing, or WiFi
  failed.
- **NODES:** one row per node: id, IP, and a box for each worker slot.
  Orange filled boxes are busy, green outlines are free. This node is marked
  `*`. A node running different firmware shows `build?`.
- **THIS NODE:** each local worker, with its task, the node that created it,
  and how long it has been running.
- **Footer:** requests outstanding, tasks run here, and results received.

The Display Pack LED shows the same state: yellow while starting, green when
idle, blue while running tasks, red on error.

## How it works

- **Discovery.** Each node multicasts a HELLO every second to
  `239.255.0.2:4301`. The HELLO carries its node id, free worker slots and
  *build id*. A node that has been silent for 5 s is dropped, and requests
  running on it fail with `CLUSTER_ERR_NODE_LOST`.
- **Build id.** This is a hash of the picoCluster version, protocol version,
  payload limit and task names. Nodes with a different build id are listed as
  incompatible and never used. Bump `CLUSTER_VER_*` when task behaviour changes.
- **Requests.** These are unicast UDP on port 4300:
  `SPAWN` → `SPAWN_ACK`, then `RESULT` → `RESULT_ACK`. Each is resent every
  200 ms until acknowledged. Workers ignore duplicate SPAWNs.
- **Placement.** `CLUSTER_NODE_ANY` picks the node, this one included, with the
  most free worker slots. Ties rotate between nodes. A task placed on this node
  runs without touching the network.
- **Limits (Pico W).** 4 workers per node. Arguments and results are up to
  512 bytes each (`CLUSTER_MAX_PAYLOAD`).
