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
