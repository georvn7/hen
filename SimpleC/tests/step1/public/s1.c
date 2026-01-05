/*
  s1 — First-Argument Byte Test (no parsing)

  Behavior:
  - If argc < 2 → return 255 (usage error: no tests/args supplied).
  - Otherwise → return the first byte of argv[1] as an unsigned char.

  Rationale:
  - Ultra-minimal argv handling for early pipeline validation.
  - Output depends on runtime input, enabling private anti–reward-hacking checks.
  - Keeps surface area tiny: no atoi/strcmp/registry—just argc and argv[1][0].
*/

int main(int argc, char *argv[]) {
    
    /* If no arguments, return error code indicating usage issue */
    if (argc < 2) {
        return 255; /* Special return code for "no tests specified" */
    }

    unsigned char first1 = (argv[1][0] != '\0') ? (unsigned char)argv[1][0] : 0;

    return (int)first1;
}
