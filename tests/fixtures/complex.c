// Source for tests/fixtures/complex.wasm.
//
// Rebuild with:
//   clang --target=wasm32 -O1 -nostdlib -Wl,--no-entry -Wl,--export-all \
//         -o complex.wasm complex.c
//
// This fixture deliberately mixes recursion, br_table, loops, 64-bit values,
// floats, memory traffic, pops/clz, and select so the differential harness sees
// more than one basic block shape.
int g_seed = 3;

int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

int switchy(int x) {
    switch (x) {
    case 0: return 11;
    case 1: return 22;
    case 2: return 33;
    case 3: return 44;
    case 4: return 55;
    case 5: return 66;
    case 6: return 77;
    default: return x * 3 + 1;
    }
}

int loops(int n) {
    int s = 0;
    for (int i = 0; i < n; ++i) {
        if (i == 3)
            continue;
        if (i > 20)
            break;
        s += i * i;
    }
    return s;
}

long long rot64(long long x, int n) {
    return (long long)((unsigned long long)x << (n & 63)) |
           (long long)((unsigned long long)x >> ((64 - n) & 63));
}

double dmath(double a, double b) { return a * b + a / (b + 1.0) - (a - b); }

float fmath2(float a, float b) { return (a * b) / (b + 2.0f) + a; }

int memsum(const int *p, int n) {
    int s = 0;
    for (int i = 0; i < n; ++i)
        s += p[i] * (i + 1);
    return s + g_seed;
}

int bitcount(unsigned x) { return __builtin_popcount(x); }

int clz(unsigned x) { return __builtin_clz(x); }

int mixed(int a, int b, int c) { return (a & b) | (c ^ (a >> 3)) ^ (b * 7); }

int sel(int c, int a, int b) { return c ? a : b; }

int f2i(float f) { return (int)f; }

float i2f(int i) { return (float)i; }

unsigned udiv(unsigned a, unsigned b) { return a / (b | 1) + a % (b | 3); }

int bigcmp(long long a, long long b) { return a < b ? -1 : a > b ? 1 : 0; }

int add_one(int x) { return x + 1; }

int double_it(int x) { return x * 2; }

int apply(int (*f)(int), int x) { return f(x); }

// A volatile function-pointer array forces a real table and element segment
// instead of letting the optimizer devirtualise the indirect call.
typedef int (*int_fn)(int);
volatile int_fn g_fns[2] = {add_one, double_it};

int use_apply(int which, int x) { return g_fns[which & 1](x); }

// Forces clang to use the shadow stack (global 0) and real memory traffic.
int stacky(int n) {
    int a[32];
    for (int i = 0; i < 32; ++i)
        a[i] = i * n;
    int s = 0;
    for (int i = 0; i < 32; ++i)
        s += a[i];
    return s;
}
