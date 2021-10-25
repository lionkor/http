# http

## How to Use

```
http-server <port>
```

Hosts the current working directory (cwd) under the specified port on the system.

## How to build

### Requirements

- A POSIXy OS (pthread, unistd.h, ...)
- A C compiler
- CMake
- Make

### Cloning

1. `git clone`

### Building

1. `cmake . -B bin`
2. `make -j $(nproc) -C bin`
3. Binary is `bin/http-server`

### Installing

With `~/.local/bin` or similar in your path, just copy or symlink `http-server` there.
