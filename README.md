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
./build            # Pico W + Display Pack -> kits/
./build D          # same, by variant name
./build tests      # host unit tests
./clean            # empty builds/, remove tests/build and kits/*.uf2
```

`PICO_SDK_PATH` selects the SDK. The default is `~/workspace/pico-sdk`.

Pico SDK 2.3.1 requires picotool 2.3.0 and finds it through CMake, not `PATH`
(the apt package is 2.1.1 and fails with "Incompatible picotool installation
found"). `./build` uses `~/workspace/picotool-install` when it exists; set
`PICOTOOL_FETCH_FROM_GIT_PATH` to use another location. To build a matching
picotool:

```bash
git clone --branch 2.3.0 https://github.com/raspberrypi/picotool.git
cmake -S picotool -B picotool/build -DPICO_SDK_PATH=$PICO_SDK_PATH \
      -DPICOTOOL_FLAT_INSTALL=1 \
      -DCMAKE_INSTALL_PREFIX=$HOME/workspace/picotool-install
make -C picotool/build install
```

Install `libusb-1.0-0-dev` first if picotool should also talk to a board over USB.

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
- `STATUS=OFF` turns off the status page on the panel, which frees the
  panel for apps. The LED still shows the cluster state. The default is
  `STATUS=ON`.
- `AUTORUN=` can name a cluster app instead of `clusterd`, for example
  `AUTORUN=mandel`. `mandel` and `cpi` start clusterd themselves if it isn't
  running (`cluster_start()` in `cluster/cluster.h`), then wait 3 s so that
  nodes booting at the same time are found.
- `BUTTONA=`, `BUTTONB=`, `BUTTONX=` and `BUTTONY=` start an app when that
  Display Pack button is pressed. This is a picoOS feature. For example,
  `BUTTONX=mandel`.
- `MANDEL_ITER=` sets mandel's iteration limit when it is started without an
  argument, which is always the case for `AUTORUN=` and `BUTTONx=` (16..1000,
  default 255).
- Keep `AUTORUN=` and `BUTTONx=` within the first 256 bytes of the file.
  picoOS only reads that far.

A node that runs the Mandelbrot when it boots, and restarts it when X is
pressed:

```
AUTORUN=mandel
BUTTONX=mandel
NODEID=0
SSID=mynetwork
PASSWORD=secret
MANDEL_ITER=500
```

The other nodes keep `AUTORUN=clusterd`. Any node with `BUTTONX=mandel` can
become the one drawing the image when you press X on it.

## Using it

From the picoOS shell on any node:

```
cluster              status: node id, IP, build id, free workers
cluster nodes        every node heard from, with free worker slots
cluster jobs         requests from this node, and tasks running on it
cluster run echo hi  run a task anywhere and print its result
cluster run echo hi 1    ...on node 1
cluster kill <req>   stop a request from this node (ids from 'cluster jobs')
run cpi 12           distributed Monte Carlo pi with 12 threads
run mandel           distributed Mandelbrot on this node's panel
run mandel 500       ...with up to 500 iterations per point (16..1000)
```

### Mandelbrot

`run mandel` draws a Mandelbrot set on the panel of the node you start it on.
`AUTORUN=mandel` and `BUTTONX=mandel` in `config.txt` start it without the
shell (see [Setting up a node](#setting-up-a-node)). With `BUTTONX=mandel`,
X still means "next view" while mandel runs. picoOS only prints
`[shell] btn: 'mandel' already running` on the console.
The image is upright when the panel is mounted portrait with the USB end at the
bottom. If your mount is the other way up, flip `MANDEL_USB_BOTTOM` in
`apps/mandel.c`.

- The image is split into 80 bands of 3 rows each. Each band is a `mandel`
  cluster task, run on whichever node has a free worker.
- A coloured stripe on the left edge of each band shows which node computed
  it.
- After each frame, the shell shows how long it took and how many bands each
  node computed.

The Display Pack buttons control it:

| Button | Action |
|--------|--------|
| A | zoom in x2 |
| B | zoom out x2 |
| X | next preset view: full set, Seahorse Valley, Elephant Valley, a spiral |
| Y | quit (also stops a frame part way through) |

**Demo mode.** If no button is pressed for a minute, mandel runs by itself.
Every 15 seconds it zooms in, as if A were pressed. After 8 zooms it moves to
the next view, as if X were pressed, and then starts zooming again. If a view
can't zoom any further, it moves on early. Pressing any button pauses the demo
for another minute, and Y still quits. The shell shows `mandel: demo ...` for
each automatic step.

While `mandel` runs, it holds the panel and the status page pauses. When it
exits, the status page comes back. If `mandel` is killed, the status page
comes back within about a second.

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
  and how long it has been running. Workers 0-3 are in the left column
  and 4-7 in the right, e.g. `0 pi     3 12s` (worker 0, task `pi`, from node
  3, running 12 s). Times over 99 s show as minutes, hours or days.
- **Footer:** requests outstanding, tasks run here, and results received.

The Display Pack LED shows the same state: yellow while starting, green when
idle, blue while running tasks, red on error.

Apps can use the panel too. An app claims it with `cl_display_claim()` and
gives it back with `cl_display_release()` (see `cluster/display.h`). While the
panel is claimed, the status page doesn't draw, and it redraws in full on
release. The LED keeps working the whole time. With `STATUS=OFF`, the panel is
cleared once after boot and then left to apps.

## How it works

- **Discovery.** Each node multicasts a HELLO every second to
  `239.255.0.2:4301`. The HELLO carries its node id, free worker slots and
  *build id*. WiFi multicast is unacknowledged and easily lost, so each node
  also sends the same HELLO by unicast to every node it already knows. Any
  packet from a node counts as a sign of life. A node that has been silent
  for 5 s is dropped, and requests running on it fail with
  `CLUSTER_ERR_NODE_LOST`. clusterd also turns off WiFi power save, which
  makes multicast more reliable.
- **Build id.** This is a hash of the picoCluster version, protocol version,
  payload limit and task names. Nodes with a different build id are listed as
  incompatible and never used. Bump `CLUSTER_VER_*` when task behaviour changes.
- **Requests.** These are unicast UDP on port 4300:
  `SPAWN` → `SPAWN_ACK`, then `RESULT` → `RESULT_ACK`. Each is resent every
  200 ms until acknowledged. Workers ignore duplicate SPAWNs.
- **Kill.** `cluster_thread_kill()` completes the request at once with
  `CLUSTER_ERR_KILLED`, and the thread must still be joined. A remote worker is
  sent `KILL` → `KILL_ACK`, resent until acknowledged. It kills the task's
  process and discards the result.
- **Placement.** `CLUSTER_NODE_ANY` picks the node, this one included, with the
  most free worker slots. Ties rotate between nodes. A task placed on this node
  runs without touching the network.
- **Limits (Pico W).** 8 workers per node (`CL_MAX_WORKERS`), and up to 32
  created-but-unjoined threads per node (`CLUSTER_MAX_PENDING`). Arguments and
  results are up to 512 bytes each (`CLUSTER_MAX_PAYLOAD`). Requires picoOS
  0.4.2 or later (16 processes, 32 threads).
