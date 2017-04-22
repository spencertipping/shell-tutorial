// fork-broken.c
#include <stdio.h>
#include <unistd.h>
int main() {
  printf("starting up (we'll get this message twice)...\n");
  printf("fork returned %d\n", fork());
  return 0;
}
