/*
  S3 — Name registry → bitmap

  Behavior
  - Each argument has the form "name[:params]". Parameters are parsed only to find ':' and are otherwise ignored.
  - Look up 'name' in a sentinel‑terminated registry (names + bit indices).
    * If found, set that bit in fail_bitmap.
    * If any name is unknown, exit 254.
  - If argc < 2 → exit 255; otherwise return fail_bitmap.

  Notes
  - Registry uses parallel arrays; string match via simple_strcmp.
*/

int simple_strcmp(const char *s1, const char *s2) {
    int i = 0;
    while (s1[i] != '\0' && s2[i] != '\0') {
        if (s1[i] != s2[i]) return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
        i++;
    }
    return (int)((unsigned char)s1[i] - (unsigned char)s2[i]);
}

/* Registry without structs: names + bit indices, sentinel-terminated with 0 */
const char *test_names[] = { "arithmetic", "logical", "pointers", 0 };
const int   bit_indices[] = { 0,           1,         2            };

int main(int argc, char *argv[])
{
    unsigned int fail_bitmap = 0;
    int i, j;

    /* If no arguments, return usage error */
    if (argc < 2) {
        return 255;
    }

    /* For each command line argument, find the corresponding test by name */
    for (i = 1; i < argc; i++) {
        int test_found = 0;
        char test_name[64];
        const char *param_str = 0; /* parsed but unused in this stage */
        int name_len = 0;

        /* Split test name and parameters (name[:params]) */
        while (argv[i][name_len] != ':' && argv[i][name_len] != '\0') {
            test_name[name_len] = argv[i][name_len];
            name_len++;
        }
        test_name[name_len] = '\0';

        if (argv[i][name_len] == ':') {
            param_str = &argv[i][name_len + 1]; /* intentionally unused here */
        }

        /* Search for the test name in our registry (simple_strcmp only) */
        for (j = 0; test_names[j] != 0; j++) {
            if (simple_strcmp(test_name, test_names[j]) == 0) {
                test_found = 1;
                /* Set the bit for this test */
                fail_bitmap |= (1U << bit_indices[j]);
                break;
            }
        }

        if (!test_found) {
            return 254; /* unknown test */
        }
    }

    /* Return the bitmap as the exit code */
    return (int)fail_bitmap;
}

