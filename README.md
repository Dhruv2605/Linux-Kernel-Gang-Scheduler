# Linux Kernel Gang Scheduling

A custom Linux kernel patch introducing the "Gang Scheduling" algorithm for precise thread co-scheduling, providing optimizations for tightly coupled parallel processes.

This project was implemented as a core modification to the Linux Kernel (`v5.15`) for the Operating Systems (OS) coursework (Assignment 2) at IIT Delhi.

## Architecture & Implementation
The repository contains modifications to the Linux kernel CPU scheduler, specifically integrating a new scheduling class (`gang_sched_class`) alongside the existing CFQ and Real-Time classes.

### Custom Kernel Components
- **`gang.c` / `gang_sched.h`**: Modular implementation of the gang scheduler class logic, defining pick-next-task behavior, enqueue/dequeue mechanics, and time-slice allocation.
- **`core.c` modifications**: Patched the main scheduler core to acknowledge and prioritize the new `gang_sched_class` when selecting tasks for the CPU runqueues.
- **`sched.h` / `sched2.h` updates**: Added missing struct definitions to `task_struct` needed to track gang IDs, member counts, and dependencies.
- **`res_usage.patch`**: A consolidated Git-diff patch file containing all structural changes made to the kernel tree, ready to be applied.

## How to Apply
To evaluate this implementation, apply the provided `.patch` file directly to your clean Linux kernel source tree.
```bash
patch -p1 < res_usage.patch
```

This will inject the new `gang.c` scheduling class into `kernel/sched/` and update the relevant headers and makefiles. Recompile your kernel to test the newly available co-scheduling routines.

## About the Author
Developed by Dhruv in his second semester at IIT Delhi (2025).
