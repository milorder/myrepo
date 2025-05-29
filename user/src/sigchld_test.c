#include "../../os/ktest/ktest.h"
#include "../lib/user.h"

// 全局变量来存储测试结果
volatile int sigchld_received = 0;
volatile int child_pid_received = 0;
volatile int child_exit_code_received = 0;
volatile int test_passed = 0;

// SIGCHLD 信号处理函数
void sigchld_handler(int signo, siginfo_t* info, void* ctx) {
    printf("SIGCHLD handler called:\n");
    printf("  signo: %d\n", signo);
    printf("  si_signo: %d\n", info->si_signo);
    printf("  si_pid: %d (child pid)\n", info->si_pid);
    printf("  si_code: %d (exit code)\n", info->si_code);
    printf("  si_status: %d\n", info->si_status);
    printf("  addr: %p\n", info->addr);
    
    // 验证 SIGCHLD 信号信息
    if (info->si_signo == SIGCHLD) {
        sigchld_received = 1;
        child_pid_received = info->si_pid;
        child_exit_code_received = info->si_code;
        
        // 在 SIGCHLD 处理函数中调用 wait 回收子进程资源
        int status;
        int waited_pid = wait(-1, &status);
        printf("wait() returned pid: %d, status: %d\n", waited_pid, status);
        
        if (waited_pid == info->si_pid && status == info->si_code) {
            test_passed = 1;
            printf("✓ SIGCHLD handler validation PASSED\n");
        } else {
            printf("✗ SIGCHLD handler validation FAILED\n");
        }
    }
}

// 测试1：子进程正常退出，验证 SIGCHLD 信号
void test_normal_exit() {
    printf("=== Test 1: Normal child exit ===\n");
    
    // 重置测试状态
    sigchld_received = 0;
    child_pid_received = 0;
    child_exit_code_received = 0;
    test_passed = 0;
    
    // 设置 SIGCHLD 处理函数
    sigaction_t sa = {
        .sa_sigaction = sigchld_handler,
        .sa_restorer = sigreturn,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, 0);
    
    printf("Parent process (pid=%d) creating child\n", getpid());
    
    int child_pid = fork();
    if (child_pid == 0) {
        // 子进程：休眠一下然后正常退出
        printf("Child process (pid=%d) running\n", getpid());
        sleep(2);
        printf("Child process (pid=%d) exiting with code 42\n", getpid());
        exit(42);
    } else {
        // 父进程：等待 SIGCHLD 信号
        printf("Parent waiting for SIGCHLD signal...\n");
        
        // 等待信号处理
        int timeout = 0;
        while (!test_passed && timeout < 20) {
            sleep(1);
            timeout++;
        }
        
        if (test_passed && sigchld_received && 
            child_pid_received == child_pid &&
            child_exit_code_received == 42) {
            printf("✓ Test 1 PASSED: Normal exit SIGCHLD works\n");
        } else {
            printf("✗ Test 1 FAILED: sigchld=%d, pid=%d (expected %d), code=%d (expected 42)\n",
                   sigchld_received, child_pid_received, child_pid, child_exit_code_received);
            exit(1);
        }
    }
}

// 测试2：子进程被 kill，验证 SIGCHLD 信号
void test_kill_exit() {
    printf("\n=== Test 2: Child killed by signal ===\n");
    
    // 重置测试状态
    sigchld_received = 0;
    child_pid_received = 0;
    child_exit_code_received = 0;
    test_passed = 0;
    
    // 设置 SIGCHLD 处理函数（继续使用之前的设置）
    
    printf("Parent process (pid=%d) creating child to be killed\n", getpid());
    
    int child_pid = fork();
    if (child_pid == 0) {
        // 子进程：无限循环等待被 kill
        printf("Child process (pid=%d) running and waiting to be killed\n", getpid());
        while (1) {
            sleep(1);
        }
        exit(1); // 永远不会到达这里
    } else {
        // 父进程：等待一会儿然后 kill 子进程
        sleep(3);
        printf("Parent killing child process %d with SIGTERM\n", child_pid);
        
        int ret = sigkill(child_pid, SIGTERM, 0);
        if (ret != 0) {
            printf("sigkill failed\n");
            exit(1);
        }
        
        // 等待 SIGCHLD 信号
        printf("Parent waiting for SIGCHLD signal after kill...\n");
        
        int timeout = 0;
        while (!test_passed && timeout < 10) {
            sleep(1);
            timeout++;
        }
        
        if (test_passed && sigchld_received && 
            child_pid_received == child_pid) {
            printf("✓ Test 2 PASSED: Kill SIGCHLD works\n");
        } else {
            printf("✗ Test 2 FAILED: sigchld=%d, pid=%d (expected %d), code=%d\n",
                   sigchld_received, child_pid_received, child_pid, child_exit_code_received);
            exit(1);
        }
    }
}

// 测试3：多个子进程退出
void test_multiple_children() {
    printf("\n=== Test 3: Multiple children ===\n");
    
    // 设置 SIGCHLD 处理函数
    sigaction_t sa = {
        .sa_sigaction = sigchld_handler,
        .sa_restorer = sigreturn,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, 0);
    
    int num_children = 3;
    int children_reaped = 0;
    
    printf("Parent creating %d children\n", num_children);
    
    for (int i = 0; i < num_children; i++) {
        int child_pid = fork();
        if (child_pid == 0) {
            // 子进程
            printf("Child %d (pid=%d) running\n", i, getpid());
            sleep(2 + i); // 错开退出时间
            printf("Child %d (pid=%d) exiting with code %d\n", i, getpid(), 100 + i);
            exit(100 + i);
        } else {
            printf("Created child %d with pid %d\n", i, child_pid);
        }
    }
    
    // 父进程等待所有子进程
    printf("Parent waiting for all children to exit...\n");
    
    while (children_reaped < num_children) {
        sleep(1);
        
        // 检查是否有子进程退出（通过 wait 非阻塞检查）
        int status;
        int waited_pid = wait(-1, &status);
        if (waited_pid > 0) {
            children_reaped++;
            printf("Reaped child pid=%d, status=%d (%d/%d)\n", 
                   waited_pid, status, children_reaped, num_children);
        }
    }
    
    printf("✓ Test 3 PASSED: Multiple children handled correctly\n");
}

int main(int argc, char *argv[]) {
    printf("=== SIGCHLD TEST SUITE ===\n");
    printf("Testing SIGCHLD signal when child processes exit\n\n");
    
    // 运行所有测试
    test_normal_exit();
    test_kill_exit();
    test_multiple_children();
    
    printf("\n=== ALL SIGCHLD TESTS PASSED ===\n");
    printf("SIGCHLD implementation is working correctly!\n");
    
    return 0;
} 