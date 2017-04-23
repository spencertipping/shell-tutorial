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
