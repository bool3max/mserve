# mserve

An mqueue-based threaded file server I'm writing to help me learn about low-level linux and POSIX concepts - processes, signals, threading with pthreads, low level IO, syscalls, etc...

By implementing threading I managed to get a >50% boost in performance (over a purely sequential, single-threaded approach) with the client program (*client.c* - spawns 10 processes that all immediately spam the file server with requests for the same file). I find these sort of performance optimizations really interesting and my end goal would be to work on them at the assembly level, though I'm still far from that :). 

Currently trying to implement a thread-pool based queue to avoid the overhead of spawning new threads as requests come in. 

---

Here are some benchmarks showing before and after results after implementing threading: 

```
time it takes 10 clients to fully receive and void a 1.7GiB file from mserve -- see client.c
I ran each implementation 11 times and took the average *not* counting the 1st time (it's always slower -- caching??)

single-threaded:                  5732.4ms @ 1.7GiB with 128KiB buffer size (i5 6600 @ 3.9Ghz, 2133Mhz RAM, kernel 5.6.7, compiled with gcc 9.3.0 w/ -O3)
multi-threaded (no thread pool):  2539.4ms @ 1.7GiB with 128KiB buffer size (i5 6600 @ 3.9Ghz, 2133Mhz RAM, kernel 5.6.7, compiled with gcc 9.3.0 w/ -O3)
```
