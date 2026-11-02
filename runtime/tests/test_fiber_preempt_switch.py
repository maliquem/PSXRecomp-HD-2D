"""A thread preempted in the exception handler resumes in place on its fiber.

Links the production traps.c and psx_fiber.c into test_fiber_preempt_switch.c at -O0 and
-O2; every other runtime seam is an abort() stub (fixture_link.py).
"""
import argparse
import tempfile
from pathlib import Path

from fixture_link import build_and_run

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as root:
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, here, here.parent, opt, Path(root),
                          ['traps.c', 'psx_fiber.c'], 'test_fiber_preempt_switch.c')
    print('PASS: a preempted thread resumes on its fiber with its own nesting (O0/O2)')
