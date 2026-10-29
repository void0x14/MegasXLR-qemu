## 2023-10-27 - [strlen in loops]
**Learning:** Checking `strlen(str)` in the condition of a `for` or `while` loop is a common anti-pattern that leads to O(N^2) complexity instead of O(N) since `strlen(str)` is evaluated on every iteration.
**Action:** When finding `strlen(str)` in loop conditions, pre-calculate it into a variable `len = strlen(str)` before the loop, or use fixed size if the length is known, to achieve O(N) complexity.

## 2024-05-19 - [g_list_append in loops]
**Learning:** Using `g_list_append` inside a loop leads to O(N^2) complexity because `g_list_append` has to traverse the entire list to find the tail pointer for each insertion.
**Action:** When building a list in a loop using GLib, use `g_list_prepend` inside the loop (which is O(1)) and `g_list_reverse` once at the end (which is O(N)) to achieve O(N) complexity overall.
