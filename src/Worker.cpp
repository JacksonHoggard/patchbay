#include "Worker.h"

#include "Fs.h"

#include <coreinit/memdefaultheap.h>
#include <cstring>

Worker::Worker(std::function<void()> fn, uint32_t stackSize) : fn_(std::move(fn)) {
    thread_ = static_cast<OSThread *>(MEMAllocFromDefaultHeapEx(sizeof(OSThread), 16));
    stack_ = MEMAllocFromDefaultHeapEx(stackSize, 16);
    bool created = false;
    if (thread_ && stack_) {
        std::memset(thread_, 0, sizeof(OSThread));
        // Stack grows down: pass the top.
        void *top = static_cast<char *>(stack_) + stackSize;
        created = OSCreateThread(thread_, &Worker::entry, 0, this, top, stackSize, 16,
                                 OS_THREAD_ATTRIB_AFFINITY_ANY);
    }
    if (!created) {
        fs::log("Worker: could not create thread (%u byte stack)", (unsigned) stackSize);
        if (thread_) MEMFreeToDefaultHeap(thread_);
        if (stack_) MEMFreeToDefaultHeap(stack_);
        thread_ = nullptr;
        stack_ = nullptr;
        joined_ = true;
        return;
    }
    OSSetThreadName(thread_, "PatchBay worker");
    OSResumeThread(thread_);
}

int Worker::entry(int, void *argv) {
    static_cast<Worker *>(argv)->fn_();
    return 0;
}

void Worker::join() {
    if (joined_) return;
    joined_ = true;
    if (thread_) {
        int ret;
        OSJoinThread(thread_, &ret);
        MEMFreeToDefaultHeap(thread_);
        MEMFreeToDefaultHeap(stack_);
        thread_ = nullptr;
        stack_ = nullptr;
    }
}

Worker::~Worker() {
    join();
}
