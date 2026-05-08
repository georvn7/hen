/*
 S0_return_constant — Minimal sanity check: if argc < 2 return 255 (usage error), otherwise always return 3.
*/

int main(int argc, char *argv[]) {
    
    if (argc < 2) {
        return 255;
    }
    
    return 0;
}
