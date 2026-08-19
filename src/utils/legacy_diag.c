#include <stdio.h>

void legacy_diag_dump(void) {
    char line[32];

   fgets(line, sizeof(line), stdin);
    puts(line);
}
