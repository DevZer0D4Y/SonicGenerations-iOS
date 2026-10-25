// Busy-wait loops in the game, made to give the core away while they wait.
//
// On a warm iPhone iOS parks the performance cores and the whole game shares
// four efficiency cores. Loops that spin on the console (six hardware threads,
// one per software thread) then take a core from the very threads they wait
// for. Both hooks are mid-assembly hooks from sonicgenerations_manifest.toml.

#include <sched.h>

#include <chrono>
#include <thread>

// Havok's job queue (0x83362D84): the submitting thread spins until the
// worker threads' completion count reaches the job count. 79% of that thread
// in Green Hill, while the workers it waits for ran on the same four cores.
void SgHavokJobWait() {
  sched_yield();
}

// D3D's wait for the GPU (0x82DB0A34, once per poll in sub_82DB09B8, which
// every ring-buffer and fence wait calls in its loop): about a third of the
// game thread. The GPU thread it waits for needs the core, and a short sleep
// costs far less heat than spinning.
void SgGpuWaitPoll() {
  std::this_thread::sleep_for(std::chrono::microseconds(30));
}
