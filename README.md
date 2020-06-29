# mserve

An mqueue-based threaded file server I'm writing to help me learn about low-level linux and POSIX concepts - processes, signals, threading with pthreads, low level IO, syscalls, etc...

By implementing threading I managed to get a ~50% boost in performance (over a purely sequential, single-threaded approach) with the client program (*client.c* - spawns 10 processes that all immediately spam the file server with requests for the same file). I find these sort of performance optimizations really interesting and my end goal would be to work on them at the assembly level, though I'm still far from that :). 

Currently trying to implement a thread-pool based queue to avoid the overhead of spawning new threads as requests come in. 

---

Here are some benchmarks showing before and after results after implementing threading: 

```
time it takes 20 clients to fully receive and void a 1.7GiB file from mserve -- see client.c
I ran each implementation 5 times and took the average *not* counting the 1st time (it's always slower -- caching??)

single-threaded:                  11043.2ms @ 1.7GiB with 128KiB buffer size
multi-threaded (no thread pool):  5851.8ms  @ 1.7GiB with 128KiB buffer size
multi-threaded (thread pool 1):   5843.2ms  @ 1.7GiB with 128KiB buffer size
multi-threaded (thread pool 2):   5891.3ms  @ 1.7GiB with 128KiB buffer size

test machine: (i5 6600 @ 3.9Ghz, 2133Mhz RAM, kernel 5.6.7, compiled with gcc 9.3.0 w/ -O3) 

each of these techniques has its own branch: 

(no thread pool - master branch): new worker threads spawn on-demand, as new requests come in
(thread pool 1) : master thread receives requests from mqueue, pushes them onto another global queue, and signals running worker threads to take care of the request. currently broken
                  (if all worker threads are busy at the time of a new request, that request will be pushed onto the global queue but will never be taken care of as worker threads only 
                  read the queue when they receive a signal from the master thread)
(thread pool 2):  N worker threads are spawned at the start. each one polls the main mqueue itself, waiting for requests. (the mqueue API is thread safe)
                  a compile time option exists which enables the spawning of new threads in the case that all worker threads are busy at the time of a new request
```
