#include "signal.h"
#include "user.h"

void alarm_handler(int signo, siginfo_t *info, void *context) {
    printf("alarm triggered! signo=%d\n", signo);
}

int main() {
    sigaction_t sa = {
        .sa_sigaction = alarm_handler,
        .sa_restorer  = sigreturn,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, 0);

    printf("setting alarm for 2 seconds...\n");
    alarm(2);
    sleep(5);  // 等待 alarm 触发
    printf("after alarm.\n");
    return 0;
}
