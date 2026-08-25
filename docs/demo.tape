# Reproducible terminal demo: the durability contract, checked by killing the
# engine mid-write a thousand times.
#
#   ./docs/demo-setup.sh && vhs docs/demo.tape

Output docs/demo.gif

Set Shell zsh
Set FontSize 15
Set Width 1440
Set Height 520
Set Padding 20
Set Theme "Builtin Dark"
Set TypingSpeed 20ms

Hide
Type "clear" Enter
Type "setopt interactive_comments" Enter
Type "alias crash_test=./build/demo/tools/crash_test" Enter
Type "clear" Enter
Show

Sleep 600ms

Type "# A storage engine's one real promise: if it acknowledged your write, the write survives."
Enter
Sleep 900ms
Type "# strata makes that mechanically checkable instead of asserted. Let's try to break it."
Enter
Sleep 1400ms

Type "crash_test orchestrate --dir /tmp/strata-demo --iters 1000 --workers 2 --fsync always --mode chain --kill bytes --seed 7"
Enter
Sleep 7000ms

Type "# Every one of those kills was real, at a random byte offset inside the engine's own write(2)."
Enter
Sleep 1600ms
Type "# Tens of thousands of acknowledged writes, all recovered. failures=0 is the entire point."
Enter
Sleep 2000ms
Type "# chain mode: each kill lands on state recovered from the kill before it."
Enter
Sleep 2000ms
Type "# Honest scope: this kills the process, not the machine. The page cache survives,"
Enter
Sleep 1200ms
Type "# so it verifies the engine's contract, not the drive's."
Enter
Sleep 2600ms
