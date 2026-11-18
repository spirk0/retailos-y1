/* Guest workload for bench/bench.c: compiled for ARMv5TE, ARM state, and
 * run by the interpreter. A mix of what retailOS code does: calls, stack
 * frames, loads and stores, byte and halfword data, branches. */
struct node { unsigned key, val; struct node *next; };

__attribute__((noinline)) static unsigned mix(unsigned a, unsigned b) {
    return (a << 5) ^ (a >> 3) ^ (b * 2654435761u);
}

__attribute__((noinline)) static void copy(unsigned short *d, const unsigned short *s, int n) {
    for (int i = 0; i < n; i++) d[i] = s[i] ^ 0x5a5a;
}

unsigned bench(unsigned *buf, int rounds) {
    struct node nodes[16];
    unsigned short pix[64];
    unsigned sum = 0;
    for (int i = 0; i < 16; i++) {
        nodes[i].key = i * 7;
        nodes[i].val = buf[i];
        nodes[i].next = &nodes[(i + 5) & 15];
    }
    for (int r = 0; r < rounds; r++) {
        struct node *p = &nodes[r & 15];
        for (int k = 0; k < 8; k++) {
            sum = mix(sum, p->val + p->key);
            if (sum & 1) p->val++;
            p = p->next;
        }
        copy(pix, (const unsigned short *)buf, 64);
        unsigned char *b = (unsigned char *)buf;
        for (int i = 0; i < 32; i++) sum += b[i] > 0x80 ? b[i] : pix[i];
        buf[r & 63] = sum;
    }
    return sum;
}
