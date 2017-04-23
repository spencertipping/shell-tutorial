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
    dup2(pipe_fds[1], 1);       // this closes the original stdout
    close(pipe_fds[0]);         // explained below
    execv("/bin/ls", ls_args);
  } else {
    // /bin/wc: do the same thing for fd 0
    dup2(pipe_fds[0], 0);
    close(pipe_fds[1]);
    execv("/usr/bin/wc", wc_args);
  }
}
