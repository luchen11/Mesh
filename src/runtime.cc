// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright 2017 University of Massachusetts, Amherst

#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/types.h>

#include "runtime.h"

namespace mesh {

// const internal::BinToken::Size internal::BinToken::Max = numeric_limits<uint32_t>::max();
// const internal::BinToken::Size internal::BinToken::MinFlags = numeric_limits<uint32_t>::max() - 4;

// const internal::BinToken::Size internal::BinToken::FlagFull = numeric_limits<uint32_t>::max() - 1;
// const internal::BinToken::Size internal::BinToken::FlagEmpty = numeric_limits<uint32_t>::max() - 2;
// const internal::BinToken::Size internal::BinToken::FlagNoOff = numeric_limits<uint32_t>::max();

__thread LocalHeap *Runtime::_localHeap;

STLAllocator<char, internal::Heap> internal::allocator{};

size_t internal::measurePssKiB() {
  size_t sz = 0;
  auto fd = open("/proc/self/smaps_rollup", O_RDONLY | O_CLOEXEC);
  if (unlikely(fd < 0)) {
    mesh::debug("measurePssKiB: no smaps_rollup");
    return 0;
  }

  static constexpr size_t BUF_LEN = 1024;
  char buf[BUF_LEN];
  memset(buf, 0, BUF_LEN);

  (void)read(fd, buf, BUF_LEN - 1);
  close(fd);

  auto start = strstr(buf, "\nPss: ");
  if (unlikely(start == nullptr)) {
    mesh::debug("measurePssKiB: no Pss");
    return 0;
  }

  return atoi(&start[6]);
}

int internal::copyFile(int dstFd, int srcFd, off_t off, size_t sz) {
#if defined(__APPLE__) || defined(__FreeBSD__)
#error FIXME: use off
  // fcopyfile works on FreeBSD and OS X 10.5+
  int result = fcopyfile(srcFd, dstFd, 0, COPYFILE_ALL);
#else
  d_assert(off >= 0);

  off_t newOff = lseek(dstFd, off, SEEK_SET);
  d_assert(newOff == off);

  errno = 0;
  // sendfile will work with non-socket output (i.e. regular file) on Linux 2.6.33+
  int result = sendfile(dstFd, srcFd, &off, sz);
#endif

  return result;
}

Runtime::Runtime() {
  createSignalFd();
}

void Runtime::createSignalFd() {
  sigset_t mask;

  sigemptyset(&mask);
  sigaddset(&mask, SIGDUMP);

  /* Block signals so that they aren't handled
     according to their default dispositions */

  if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
    debug("failed to block signal mask for BG thread");
    abort();
  }

  _signalFd = signalfd(-1, &mask, 0);
  if (_signalFd == -1) {
    debug("failed to create signal FD");
    abort();
  }
}

void Runtime::startBgThread() {
  constexpr int MaxRetries = 20;

  pthread_t bgPthread;
  int retryCount = 0;
  int ret = 0;

  while ((ret = pthread_create(&bgPthread, nullptr, Runtime::bgThread, nullptr))) {
    retryCount++;
    sched_yield();

    if (retryCount % 10)
      debug("background thread creation failed, retrying.\n");

    if (retryCount >= MaxRetries) {
      debug("max retries exceded: couldn't create bg thread, exiting.\n");
      abort();
    }
  }
}

void *Runtime::bgThread(void *arg) {
  auto &rt = mesh::runtime();

  // debug("libmesh: background thread started\n");

  while (true) {
    struct signalfd_siginfo siginfo;

    ssize_t s = read(rt._signalFd, &siginfo, sizeof(struct signalfd_siginfo));
    if (s != sizeof(struct signalfd_siginfo)) {
      if (s >= 0) {
        debug("bad read size: %lld\n", s);
        abort();
      } else {
        // read returns -1 if the program gets a process-killing signal
        return nullptr;
      }
    }

    if (static_cast<int>(siginfo.ssi_signo) == SIGDUMP) {
      // debug("libmesh: background thread received SIGDUMP, starting dump\n");
      // debug(">>>>>>>>>>\n");
      rt.heap().dumpStrings();
      // debug("<<<<<<<<<<\n");
    } else {
      printf("Read unexpected signal\n");
    }
  }
}

void Runtime::lock() {
  _mutex.lock();
}

void Runtime::unlock() {
  _mutex.unlock();
}

void Runtime::allocLocalHeap() {
  d_assert(_localHeap == nullptr);

  void *buf = mesh::internal::Heap().malloc(sizeof(LocalHeap));
  if (buf == nullptr) {
    mesh::debug("mesh: unable to allocate LocalHeap, aborting.\n");
    abort();
  }
  _localHeap = new (buf) LocalHeap(&mesh::runtime().heap());
}
}  // namespace mesh
