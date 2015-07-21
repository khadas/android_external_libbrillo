// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chromeos/process_reaper.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <base/bind.h>
#include <base/location.h>
#include <base/message_loop/message_loop.h>
#include <chromeos/asynchronous_signal_handler.h>
#include <chromeos/bind_lambda.h>
#include <chromeos/message_loops/base_message_loop.h>
#include <gtest/gtest.h>

namespace {

pid_t ForkChildAndExit(int exit_code) {
  pid_t pid = fork();
  PCHECK(pid != -1);
  if (pid == 0) {
    _exit(exit_code);
  }
  return pid;
}

pid_t ForkChildAndKill(int sig) {
  pid_t pid = fork();
  PCHECK(pid != -1);
  if (pid == 0) {
    if (raise(sig) != 0) {
      PLOG(ERROR) << "raise(" << sig << ")";
    }
    _exit(0);  // Not reached. This value will cause the test to fail.
  }
  return pid;
}

}  // namespace

namespace chromeos {

class ProcessReaperTest : public ::testing::Test {
 public:
  void SetUp() override {
    chromeos_loop_.SetAsCurrent();
    async_signal_handler_.Init();
    process_reaper_.RegisterWithAsynchronousSignalHandler(
        &async_signal_handler_);
  }

 protected:
  base::MessageLoopForIO base_loop_;
  chromeos::BaseMessageLoop chromeos_loop_{&base_loop_};
  chromeos::AsynchronousSignalHandler async_signal_handler_;

  // ProcessReaper under test.
  ProcessReaper process_reaper_;
};

TEST_F(ProcessReaperTest, UnregisterWhenNotRegistered) {
  ProcessReaper another_process_reaper_;
  another_process_reaper_.Unregister();
}

TEST_F(ProcessReaperTest, UnregisterAndReregister) {
  process_reaper_.Unregister();
  process_reaper_.RegisterWithAsynchronousSignalHandler(
      &async_signal_handler_);
  // This checks that we can unregister the ProcessReaper and then destroy it.
  process_reaper_.Unregister();
}

TEST_F(ProcessReaperTest, ReapExitedChild) {
  pid_t pid = ForkChildAndExit(123);
  EXPECT_TRUE(process_reaper_.WatchForChild(FROM_HERE, pid, base::Bind(
      [this](const siginfo_t& info) {
        EXPECT_EQ(CLD_EXITED, info.si_code);
        EXPECT_EQ(123, info.si_status);
        this->chromeos_loop_.BreakLoop();
      })));
  chromeos_loop_.Run();
}

// Test that simultaneous child processes fire their respective callbacks when
// exiting.
TEST_F(ProcessReaperTest, ReapedChildsMatchCallbacks) {
  int running_childs = 10;
  for (int i = 0; i < running_childs; ++i) {
    // Different processes will have different exit values.
    int exit_value = 1 + i;
    pid_t pid = ForkChildAndExit(exit_value);
    EXPECT_TRUE(process_reaper_.WatchForChild(FROM_HERE, pid, base::Bind(
        [this, exit_value, &running_childs](const siginfo_t& info) {
          EXPECT_EQ(CLD_EXITED, info.si_code);
          EXPECT_EQ(exit_value, info.si_status);
          running_childs--;
          if (running_childs == 0)
            this->chromeos_loop_.BreakLoop();
        })));
  }
  // This sleep is optional. It helps to have more processes exit before we
  // start watching for them in the message loop.
  usleep(10 * 1000);
  chromeos_loop_.Run();
  EXPECT_EQ(0, running_childs);
}

TEST_F(ProcessReaperTest, ReapKilledChild) {
  pid_t pid = ForkChildAndKill(SIGKILL);
  EXPECT_TRUE(process_reaper_.WatchForChild(FROM_HERE, pid, base::Bind(
      [this](const siginfo_t& info) {
        EXPECT_EQ(CLD_KILLED, info.si_code);
        EXPECT_EQ(SIGKILL, info.si_status);
        this->chromeos_loop_.BreakLoop();
      })));
  chromeos_loop_.Run();
}

}  // namespace chromeos
