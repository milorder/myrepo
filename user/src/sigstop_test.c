#include "../../os/ktest/ktest.h"
#include "../lib/user.h"

// 用于测试的全局计数器
volatile int counter = 0;
volatile int test_status = 0;

void test_handler(int sig, siginfo_t *info, void *ucontext) {
    printf("Signal handler called for signal %d\n", sig);
    printf("  si_signo: %d\n", info->si_signo);
    printf("  si_pid: %d\n", info->si_pid);
    printf("  si_code: %d\n", info->si_code);
    test_status = 1;
}

// 测试SIGSTOP和SIGCONT基本功能
void test_basic_stop_cont() {
    printf("=== Test 1: Basic SIGSTOP/SIGCONT ===\n");
    
    int pid = fork();
    if (pid == 0) {
        // 子进程：运行一个计数循环
        printf("Child process (pid=%d) starting counter\n", getpid());
        for (int i = 0; i < 20; i++) {
            printf("Child counter: %d\n", i);
            counter = i;
            sleep(1); // 模拟工作
        }
        printf("Child finished counting\n");
        exit(counter);
    } else {
        // 父进程：控制子进程的暂停和恢复
        printf("Parent process (pid=%d) created child (pid=%d)\n", getpid(), pid);
        
        // 让子进程运行一会儿
        sleep(3);
        printf("Parent sending SIGSTOP to child...\n");
        sigkill(pid, SIGSTOP, 0);
        
        // 暂停5秒
        printf("Child should be stopped now, waiting 5 seconds...\n");
        sleep(5);
        
        printf("Parent sending SIGCONT to child...\n");
        sigkill(pid, SIGCONT, 0);
        
        // 等待子进程完成
        int status;
        int child_pid = wait(pid, &status);
        printf("Child process %d finished with status %d\n", child_pid, status);
        
        if (child_pid == pid && status >= 0) {
            printf("✓ Test 1 PASSED: Basic SIGSTOP/SIGCONT works\n");
        } else {
            printf("✗ Test 1 FAILED: Unexpected result\n");
        }
    }
    printf("\n");
}

// 测试多次暂停和恢复
void test_multiple_stop_cont() {
    printf("=== Test 2: Multiple SIGSTOP/SIGCONT ===\n");
    
    int pid = fork();
    if (pid == 0) {
        // 子进程：运行计数器，每次被恢复时继续
        printf("Child (pid=%d) running with multiple stops\n", getpid());
        for (int i = 0; i < 15; i++) {
            printf("Child working: %d\n", i);
            sleep(1);
        }
        printf("Child completed all work\n");
        exit(42);
    } else {
        // 父进程：多次暂停和恢复子进程
        printf("Parent will stop and continue child multiple times\n");
        
        // 第一次暂停和恢复
        sleep(2);
        printf("First SIGSTOP...\n");
        sigkill(pid, SIGSTOP, 0);
        sleep(2);
        printf("First SIGCONT...\n");
        sigkill(pid, SIGCONT, 0);
        
        // 第二次暂停和恢复
        sleep(2);
        printf("Second SIGSTOP...\n");
        sigkill(pid, SIGSTOP, 0);
        sleep(2);
        printf("Second SIGCONT...\n");
        sigkill(pid, SIGCONT, 0);
        
        // 等待完成
        int status;
        int child_pid = wait(pid, &status);
        printf("Child %d finished with status %d\n", child_pid, status);
        
        if (child_pid == pid && status == 42) {
            printf("✓ Test 2 PASSED: Multiple SIGSTOP/SIGCONT works\n");
        } else {
            printf("✗ Test 2 FAILED: Expected status 42, got %d\n", status);
        }
    }
    printf("\n");
}

// 测试SIGCONT对未暂停进程的影响
void test_cont_running_process() {
    printf("=== Test 3: SIGCONT to running process ===\n");
    
    int pid = fork();
    if (pid == 0) {
        // 子进程：运行并处理信号
        sigaction_t sa = {
            .sa_sigaction = test_handler,
            .sa_restorer = sigreturn,
        };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, 0);
        
        printf("Child (pid=%d) running normally\n", getpid());
        
        // 运行一段时间
        for (int i = 0; i < 10; i++) {
            printf("Child normal execution: %d\n", i);
            sleep(1);
            if (test_status) break; // 如果收到测试信号就退出
        }
        
        printf("Child normal execution finished\n");
        exit(0);
    } else {
        // 父进程：向运行中的子进程发送SIGCONT
        printf("Parent sending SIGCONT to running child...\n");
        sleep(1);
        sigkill(pid, SIGCONT, 0); // 应该对运行中的进程无影响
        
        sleep(2);
        printf("Parent sending SIGUSR1 to signal end of test...\n");
        sigkill(pid, SIGUSR1, 123);
        
        int status;
        int child_pid = wait(pid, &status);
        printf("Child %d finished with status %d\n", child_pid, status);
        
        if (child_pid == pid && status == 0) {
            printf("✓ Test 3 PASSED: SIGCONT to running process handled correctly\n");
        } else {
            printf("✗ Test 3 FAILED: Unexpected result\n");
        }
    }
    printf("\n");
}

int main() {
    printf("=== SIGSTOP & SIGCONT TEST SUITE ===\n");
    printf("Testing process suspension and resumption\n\n");
    
    test_basic_stop_cont();
    test_multiple_stop_cont();
    test_cont_running_process();
    
    printf("=== ALL SIGSTOP/SIGCONT TESTS COMPLETED ===\n");
    printf("SIGSTOP/SIGCONT implementation test finished!\n");
    
    return 0;
} 