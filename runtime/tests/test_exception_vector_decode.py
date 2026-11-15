"""Compile the exception-vector decode from interrupts.c and check LUI/ADDIU gating."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--src-root', type=Path,
                        default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    text = (args.src_root/'src/interrupts.c').read_text(encoding='utf-8-sig')
    start = text.index('    uint32_t target_pc;\n    if (sr & 0x00400000u)')
    block = text[start:text.index('/* Record which fiber owns', start)]
    fixture = r'''
#include <stdint.h>
#include <stdio.h>
typedef struct { uint32_t gpr[32]; uint32_t (*read_word)(uint32_t); } CPUState;
static uint32_t w0, w1;
static uint32_t rd(uint32_t a) { return a==0x80000080u ? w0 : w1; }
static uint32_t decode(CPUState *cpu, uint32_t sr) {
BLOCK
    return target_pc;
}
static unsigned failures;
static void run(uint32_t a, uint32_t b, uint32_t sr, uint32_t want, int k0set) {
    CPUState cpu = {{0}, rd};
    w0 = a; w1 = b;
    uint32_t got = decode(&cpu, sr);
    if (got != want || (cpu.gpr[26] != 0) != k0set || (k0set && cpu.gpr[26] != want)) {
        printf("case %08x %08x: pc %08x k0 %08x\n", a, b, got, cpu.gpr[26]); ++failures;
    }
}
int main(void) {
    run(0x3C1A8001u, 0x275A0C80u, 0, 0x80010C80u, 1);          /* lui k0 / addiu k0,k0 */
    run(0x3C1A8001u, 0x275AF000u, 0, 0x8000F000u, 1);          /* negative lo */
    run(0x0BC00000u, 0x00000000u, 0, 0x80000080u, 0);          /* j + nop */
    run(0x3C1A8001u, 0x375A0C80u, 0, 0x80000080u, 0);          /* lui + ori */
    run(0x3C1B8001u, 0x275A0C80u, 0, 0x80000080u, 0);          /* addiu rs != lui rt */
    run(0x3C1A8001u, 0x275A0C80u, 0x00400000u, 0xBFC00180u, 0); /* BEV */
    printf("%u failures\n", failures);
    return failures ? 1 : 0;
}
'''.replace('BLOCK', block)
    options = dict(stdin=subprocess.DEVNULL, capture_output=True, text=True,
                   encoding='utf-8', errors='replace', timeout=60)
    with tempfile.TemporaryDirectory() as directory:
        unit = Path(directory)/'vec.c'
        exe = Path(directory)/'vec.exe'
        unit.write_text(fixture, encoding='utf-8')
        for opt in ['-O0', '-O2']:
            r = subprocess.run([args.cc, '-std=c11', opt, str(unit), '-o', str(exe)], **options)
            if r.returncode:
                raise RuntimeError(r.stdout+r.stderr)
            r = subprocess.run([str(exe)], **options)
            print(opt, r.stdout, end='')
            if r.returncode:
                return r.returncode
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
