## 2023-10-27 - [strlen in loops]
**Learning:** Checking `strlen(str)` in the condition of a `for` or `while` loop is a common anti-pattern that leads to O(N^2) complexity instead of O(N) since `strlen(str)` is evaluated on every iteration.
**Action:** When finding `strlen(str)` in loop conditions, pre-calculate it into a variable `len = strlen(str)` before the loop, or use fixed size if the length is known, to achieve O(N) complexity.

## 2024-11-20 - [g_list_append in loops]
**Learning:** In GLib codebases, `g_list_append` iterates over the entire list to find the end, making it an O(N) operation. When used inside a loop, it leads to O(N^2) complexity.
**Action:** When building a list inside a loop, always use `g_list_prepend` (which is O(1)) inside the loop, and call `g_list_reverse` (O(N)) once after the loop finishes.
