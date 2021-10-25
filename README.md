# http

A small http server written in C.

Don't use in production environments, don't run as root user, etc. 

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


<hr>

I'm not liable for any damage caused by this program or any of its components, not responsible for anything you do with it, and there's no warranty of any kind on it.
Please, for the love of god, don't actually use this as a http server. It's just a hobby project.
