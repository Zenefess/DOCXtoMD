Counters run over the whole document.

1. First.
2. Second.

An interruption, which the counters do not notice.

3. Third, which continues where the second left off.
4. A second numId over the same definition continues it too.
<!-- -->
1. A startOverride restarts it, once.
2. Its second item counts on from the restart.

Levels count on their own, and a deeper one is cleared by a shallower.

1. Outer.
   1. Inner.
      1. Innermost.
   2. Inner again.

      2. Innermost again, which lvlRestart 0 refuses to restart.

Continuations.

1. An item.

   A paragraph inside it, which numFmt none leaves unmarked.
2. The next item.

A nested item between two lists must not hide the restart.

1. One.
   1. A nested item.
<!-- -->
1. A restart, which the block above it stands between.

A numId first used deep restarts its shallow levels there, not where they are reached.

4. Alpha.
   1. Beta, whose numId spends its startOverride here.
<!-- -->
1. Gamma, which begins a list although nothing restarted at it.
