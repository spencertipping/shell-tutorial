// fork.c
#include <stdio.h>
#include <unistd.h>
int main() {
  printf("starting up...\n");
  fflush(stdout);
  printf("fork returned %d\n", fork());
  return 0;
}
