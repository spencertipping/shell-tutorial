# How to write a (very basic) UNIX shell
UNIX shells are pretty much unique in the ease with which they let you
manipulate the state of your system. Doing things which are trivial in a shell
script requires a fair amount of code in most other languages, and that's
probably in part why the shell has been such an enduring tool for the past few
decades.

But such historical appreciation simply breeds complacency; it's time for
something better than `/bin/bash`. Before writing it, though, it's probably
useful to understand what `bash` is doing internally. That's what this guide is
about.

## What happens when you run a command
Ok before we get into this, let's talk about what `bash` (or `sh`) does when
you run a simple command like `ls`.

```sh
$ ls                    # <- what's really happening here?
README.md               # <- and how did this get to the screen?
$
```

### The terminal emulator and PTY devices
The first thing to talk about is how your keystrokes end up getting sent to
bash. You're most likely running a terminal emulator, which is a fairly normal
graphical program that owns the shell process; internally, it's set up like
this:

```
                                              +----------------------------+
   +---------------------------+              |                            |
   |                           |  pty device  +-fd 0 (stdin)               |
   |  terminal emulator   fd N-+--------------+-fd 1 (stdout)   /bin/bash  |
   |  (e.g. xterm, iterm)      |              +-fd 2 (stderr)              |
   |                           |              |                            |
   +---------------------------+              +----------------------------+
```

The PTY device behaves like a bidirectional data pipe (a socket): if you write
into one end, you can read from the other end. It has some other attributes
that define its state, but fundamentally it's just a two-way pipe. So the
terminal emulator listens for both keyboard events and for available data on
the PTY device, and either writes or updates the screen accordingly.

`bash`, or whichever shell you're running, is started up with file descriptors
0, 1, and 2 mapped to the other end of the PTY device. UNIX convention dictates
that processes read from fd 0 and write to fds 1 and 2, but the kernel really
doesn't care what you do with any of them since they all point to the same IO
resource. For example, we can read from stderr and write to stdin:

```sh
$ cat >&0 <&2
foo                     # I typed this
foo                     # ...and got this
$
```

As far as the kernel is concerned, `cat` is reading from the PTY device and
writing to it and it's business as usual.

### `fork` and `exec`
Booting a modern Linux or OSX doesn't feel minimalistic at all, but internally
the mechanism for it is surprisingly elegant. The kernel starts off by creating
a single process, usually called `init` and with
[PID](https://en.wikipedia.org/wiki/Process_identifier) 1, and this process
then clones itself a bunch of times using the `fork` system call. The clones
call `exec` to turn themselves into other programs, which themselves call
`fork` and `exec` until you've got terminal emulators, shells, and
subprocesses.

Let's start by looking at `fork`:

```c
// fork.c
#include <stdio.h>
#include <unistd.h>
int main() {
  printf("starting up...\n");
  fflush(stdout);
  printf("fork returned %d\n", fork());
  return 0;
}
```

(I'll explain `fflush` in a minute...)

```sh
$ c99 -o fork fork.c
$ ./fork
starting up...                  # started once...
fork returned 11182             # and returned
fork returned 0                 # ...twice!
```

`fork` [is
defined](http://pubs.opengroup.org/onlinepubs/000095399/functions/fork.html) to
clone the calling process and return from each independently. The parent
process gets the nonzero PID of the child process, and the child gets the
return value of `0`. The two processes are linked from the kernel's point of
view: the child's "parent process ID" refers to the parent, which means the
parent can (and is expected to) ask about its exit status.

An important aspect of `fork` is that the parent and child don't share any
memory; any modifications made inside one won't be visible to the other. This
is why we need `fflush` in the code above: `printf()` internally writes to a
buffer before issuing the `write` call to the kernel, and if there are buffered
contents then we'll see them twice since the buffered contents are copied along
with other memory. For example:

```c
// fork-broken.c
#include <stdio.h>
#include <unistd.h>
int main() {
  printf("starting up (we'll get this message twice)...\n");
  printf("fork returned %d\n", fork());
  return 0;
}
```

```sh
$ c99 -o fork-broken fork-broken.c
$ ./fork-broken
starting up (we'll get this message twice)...
fork returned 28164
starting up (we'll get this message twice)...
fork returned 0
```

Another important detail is that the child process inherits file descriptors
from the parent -- though subsequent modifications, just like memory, aren't
shared. But this is why both `printf` calls went to the same terminal.

### `exec`, address spaces, and executables
[`exec`](https://en.wikipedia.org/wiki/Exec_(system_call)) means "turn into
another program," which brings up a more fundamental question: "what does it
mean to 'be' a program?"

Let's start with the executable file format. Linux uses the [ELF
format](https://en.wikipedia.org/wiki/Executable_and_Linkable_Format) for
binaries -- other OSes use [files with similar
concepts](https://en.wikipedia.org/wiki/COFF) but different
implementation details. The basic pieces are (see `man elf`):

```
e_ident                 # magic number for ELF files
e_machine...            # some bytes specifying the machine code architecture
e_entry                 # entry point (start address) within virtual memory
...
program_headers...      # a list of program memory mappings
  memory_address        # location within virtual memory
  file_address          # offset within this file
...
```

There are other fields used for things like debugging symbols, but the fields
above are what governs the executable process itself. (For a more concrete
example of how this works, check out
[tinyelf](https://github.com/spencertipping/tinyelf), which reduces compiled C
programs down to minimal ELF files.)

So when you call `exec`, the kernel first resets the [virtual memory
space](https://en.wikipedia.org/wiki/Virtual_memory) for the process, then goes
through the program headers and sets up memory mappings into the executable
file. Once that's done, it points the processor at `e_entry` to transfer
control to the program.

`exec` also does a few other things to manage state, like closing file
descriptors marked with the `FD_CLOEXEC` flag and resetting signal handlers.
I'll cover the relevant details as we get into the shell logic.

## Ok, shell time
With that background the basic structure of a shell should be a little clearer.
We can start by writing a "shell" hard-coded to run `ls`:

```c
// ls-shell.c
#include <stdio.h>
#include <unistd.h>
int main() {
  char *const ls_argv[] = { "/bin/ls", NULL };
  if (!fork()) {
    // we're the child: execute ls
    execv("/bin/ls", ls_argv);
    perror("uh oh, execv() returned\n");
    return 1;
  }

  // we're the parent
  printf("hope that worked\n");
  return 0;
}
```

```sh
$ c99 -o ls-shell ls-shell.c
$ ./ls-shell
hope that worked
README.md
fork
fork-broken
fork-broken.c
fork.c
ls-shell
ls-shell.c
```

### `argv` support
`argv` is specified during the `exec()` call, and those arguments are forwarded
by the kernel as `char* argv[]` to the new program. The exact mechanism for
this is specified by the operating system's ABI and handled by `libc` before it
calls into `main()`.

We could execute `ls -l` by specifying an argument after the program name. The
program name is always the first entry in `argv` because otherwise a program
wouldn't know where it existed in the filesystem. Most programs don't rely on
you to set `argv[0]` accurately, though some things like
[BusyBox](https://en.wikipedia.org/wiki/BusyBox) use it to figure out which
program you intended to run (since they're all linked to the same file).

While we're at it, let's go ahead and handle user input too.

```c
// argv-shell.c
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main() {
  char   *words[256];
  char   *line      = NULL;
  size_t  line_size = 0;
  ssize_t n;
  pid_t   child;
  int     child_status;

  while ((n = getline(&line, &line_size, stdin)) > 0) {
    // Erase ending newline by shortening the string by one
    line[n - 1] = '\0';

    // Split the line into argv words on spaces
    words[0] = line;
    for (int i = 1; words[i] = strchr(words[i - 1], ' '); ++i)
      *(words[i]++) = '\0';

    if (child = fork()) {
      // Parent process: wait for the child
      waitpid(child, &child_status, 0);
      fprintf(stderr, "child exited with status %d\n", child_status);
      fflush(stderr);
    } else {
      // Child process: exec or complain
      execv(words[0], words);
      perror("execv() failed");
      return 1;
    }
  }
}
```

Now we have a functioning, if awful, shell:

```sh
$ c99 -o argv-shell argv-shell.c
$ ./argv-shell
/bin/ls
README.md  argv-shell.c  fork-broken    fork.c    ls-shell.c
a.out      fork          fork-broken.c  ls-shell
child exited with status 0
/bin/ls -l
total 76
-rw-r--r-- 1 spencertipping spencertipping 9313 Apr 23 16:10 README.md
-rwxr-xr-x 1 spencertipping spencertipping 9120 Apr 23 16:11 a.out
-rw-r--r-- 1 spencertipping spencertipping  964 Apr 23 16:11 argv-shell.c
-rwxr-xr-x 1 spencertipping spencertipping 8800 Apr 22 11:09 fork
-rwxr-xr-x 1 spencertipping spencertipping 8712 Apr 22 12:02 fork-broken
-rw-r--r-- 1 spencertipping spencertipping  184 Apr 22 12:02 fork-broken.c
-rw-r--r-- 1 spencertipping spencertipping  164 Apr 22 11:35 fork.c
-rwxr-xr-x 1 spencertipping spencertipping 8816 Apr 23 09:21 ls-shell
-rw-r--r-- 1 spencertipping spencertipping  324 Apr 23 09:21 ls-shell.c
child exited with status 0
```

Awful things at this point include:

1. The shell doesn't search through `$PATH` entries, which means we have to
   type out the full path of the program we want to execute.
2. Multiple spaces in a row will produce empty `argv` entries, due to the way
   we're parsing stuff.
3. It provides no support for redirection of any kind.

(1) and (2) are beyond the scope of this tutorial -- but you can see the
solution to (1) by running `strace` on `/bin/sh`:

```sh
$ strace /bin/sh -c ls 2>&1 | grep stat
stat("/usr/local/sbin/ls", 0x7ffc8df82ce0) = -1 ENOENT (No such file or directory)
stat("/usr/local/bin/ls", 0x7ffc8df82ce0) = -1 ENOENT (No such file or directory)
stat("/usr/sbin/ls", 0x7ffc8df82ce0)    = -1 ENOENT (No such file or directory)
stat("/usr/bin/ls", 0x7ffc8df82ce0)     = -1 ENOENT (No such file or directory)
stat("/sbin/ls", 0x7ffc8df82ce0)        = -1 ENOENT (No such file or directory)
stat("/bin/ls", {st_mode=S_IFREG|0755, st_size=126584, ...}) = 0
```

Basically, the shell is looking at each `$PATH` entry and calling `stat()` to
see if those files exist and are executable. When it finds one that is, it
backfills the path and calls `exec`.

#### Why we wait for child processes
The common answer is that "you'll accumulate zombies and this is bad," but how
bad can it really be? By the time a program exits, it can't possibly use much
memory. Why do we care?

For short-lived code it really doesn't matter much. Once you exit, any child
processes you've created will be adopted by `init`, which will wait for them to
exit and nothing bad happens. The problem is what happens if you keep running.

The PID returned by `fork` is a promise from the kernel that until you collect
the child process, the PID will refer to that process. That is, the kernel
isn't at liberty to reuse the PID until you've called `wait` or `waitpid` to
free it. If the kernel did reuse PIDs without acknowledgement, you'd run into
race conditions where you could send a signal "to your child process" -- but
that process had exited and been replaced by something else.

So zombie processes aren't a memory issue, they're a PID issue. Most systems
don't have that many:

```sh
$ cat /proc/sys/kernel/max_pid
32768
```

### Pipelines
Right now we can run `/bin/ls` and `/usr/bin/wc -l`, but we can't pipe one into
the other. To fix this we'll need to create a pipe and then hook the
reading-end up to stdin on the second process, and the writing-end up to stdout
of the first.

Here's what this looks like:

```c
// simple-pipe.c
#include <stdio.h>
#include <unistd.h>
int main() {
  int pipe_fds[2];              // (read_end, write_end)
  pipe(pipe_fds);
  char *ls_args[] = { "/bin/ls", NULL };
  char *wc_args[] = { "/usr/bin/wc", "-l", NULL };
  if (fork()) {
    // /bin/ls: replace stdout (fd 1) with the write end of the pipe
    dup2(pipe_fds[1], 1);       // alias pipe_fds[1] to fd 1
    close(pipe_fds[1]);         // remove pipe_fds[1] from fd table
    close(pipe_fds[0]);         // explained below
    execv("/bin/ls", ls_args);
  } else {
    // /bin/wc: do the same thing for fd 0
    dup2(pipe_fds[0], 0);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    execv("/usr/bin/wc", wc_args);
  }
}
```

```sh
$ c99 -o simple-pipe simple-pipe.c
$ ./simple-pipe
11
```

#### Why we close file descriptors (and what this does)
When I was first learning about this stuff I assumed `close()` must have some
effect on the underlying IO device, but this isn't true. This will make sense
if you look at it from the kernel's point of view.

```
  /bin/ls               /usr/bin/wc -l
    fd 0 -----------+     fd 0 -----------+
    fd 1 --------+  |     fd 1 --------+  |
    fd 2 -----+  |  |     fd 2 -----+  |  |
              |  |  |               |  |  |        userspace
============================================================
              |  |  |               |  |  |     kernel space
              |  |  |               |  |  |
              |  /  |               |  |  /
              +-(---+---------------+--+-(-----  pty device (4 references)
                 \                        \
                 |                        |
                 +------------------------+---- pipe device (2 references)
```

Closing a file descriptor will cause the kernel to remove an entry from a
process's FD table, but the underlying device won't know the difference. The
device is impacted only when no program refers to it; at that point the kernel
will deallocate it. `dup2` is the same kind of thing: it creates a new FD table
reference within a program, but otherwise doesn't change the IO picture.

# TODO
- Pipelines in a shell (like more than one of them)
- File redirection example
- Network redirection example
- Background jobs, process groups, and weird `SIGTTOU` stuff
- Conditionals
- How to parse this stuff without using `strchr`?
- Suggestive commentary about how JIT for a shell would be the best thing ever
