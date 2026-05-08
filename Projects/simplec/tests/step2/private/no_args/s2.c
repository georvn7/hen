/*
  S2 — Count '1' across arguments (standalone)

  Behavior
  - If argc < 2 → exit 255.
  - Let base = first byte of argv[1] as unsigned (0 if argv[1] is empty).
  - Scan every argument argv[i] (i ≥ 1); count how many characters are the digit '1'.
  - Return (unsigned char)(base + total_count_of_'1's).

  Implementation notes
  - One for‑loop over argv and a while‑loop over characters; no helpers or registry.
*/

int main(int argc, char *argv[]) {
    if (argc < 2) return 255;

    unsigned char base = (argv[1][0] != '\0') ? (unsigned char)argv[1][0] : 0;

    int total_ones = 0;
    int i;
    for (i = 1; i < argc; i++) {
        int k = 0;
        while (argv[i][k] != '\0') {
            if (argv[i][k] == '1') total_ones++;
            k++;
        }
    }

    int result = (int)base + total_ones;
    return (unsigned char)result;
}
