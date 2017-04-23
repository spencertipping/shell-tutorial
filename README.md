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
