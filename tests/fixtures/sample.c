// Source for tests/fixtures/sample.wasm.
//
// Rebuild with:
//   clang --target=wasm32 -O1 -nostdlib -Wl,--no-entry -Wl,--export-all \
//         -o sample.wasm sample.c
//
// The test harness runs every exported function here through both the WASM
// oracle and the lifted IL and compares the results.
int g_counter = 7;

int add(int a, int b) { return a + b; }

int sum_to(int n) {
    int s = 0;
    for (int i = 0; i < n; ++i)
        s += i;
    return s;
}

int branchy(int x) {
    switch (x) {
    case 0: return 1;
    case 1: return 3;
    case 2: return 5;
    default: return x * 2;
    }
}

int memtest(int *p, int n) {
    int s = 0;
    for (int i = 0; i < n; ++i)
        s += p[i];
    return s + g_counter;
}

long long big(long long a, long long b) {
    return (a * b) + (a / (b | 1));
}

float fmath(float a, float b) {
    return a * b + a / (b + 1.0f);
}
