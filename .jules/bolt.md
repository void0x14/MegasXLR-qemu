## 2023-10-27 - [strlen in loops]
**Learning:** Checking `strlen(str)` in the condition of a `for` or `while` loop is a common anti-pattern that leads to O(N^2) complexity instead of O(N) since `strlen(str)` is evaluated on every iteration.
**Action:** When finding `strlen(str)` in loop conditions, pre-calculate it into a variable `len = strlen(str)` before the loop, or use fixed size if the length is known, to achieve O(N) complexity.
