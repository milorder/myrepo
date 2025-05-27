#include "ksignal.h"

#include <defs.h>
#include <proc.h>
#include <trap.h>

extern uint ticks;
extern struct spinlock tickslock;

int do_alarm(int seconds) {
    struct proc *p = myproc();
    int old        = p->alarm_active ? p->alarm_seconds : 0;

    if (seconds == 0) {
        p->alarm_active  = 0;
        p->alarm_ticks   = 0;
        p->alarm_seconds = 0;
        return old;
    }

    acquire(&tickslock);
    p->alarm_ticks = ticks + seconds;
    release(&tickslock);

    p->alarm_active  = 1;
    p->alarm_seconds = seconds;
    return old;
}

int sys_alarm(void) {
    int seconds;
    if (argint(0, &seconds) < 0)
        return -1;
    return do_alarm(seconds);
}

// 初始化信号
int siginit(struct proc *p) {
    for (int i = SIGMIN; i <= SIGMAX; i++) {
        p->signal.sa[i].sa_sigaction = SIG_DFL;
        sigemptyset(&p->signal.sa[i].sa_mask);
        p->signal.sa[i].sa_restorer = NULL;
    }
    sigemptyset(&p->signal.sigmask);
    sigemptyset(&p->signal.sigpending);
    return 0;
}
// 复制父进程的信号处理配置
int siginit_fork(struct proc *parent, struct proc *child) {
    memmove(&child->signal.sa[1], &parent->signal.sa[1], sizeof(struct sigaction) * SIGMAX);
    child->signal.sigmask = parent->signal.sigmask;
    sigemptyset(&child->signal.sigpending);  // 清空挂起信号
    return 0;
}
// 重置信号处理方式（保留被忽略的）
int siginit_exec(struct proc *p) {
    for (int i = SIGMIN; i <= SIGMAX; i++) {
        if (p->signal.sa[i].sa_sigaction != SIG_IGN) {
            p->signal.sa[i].sa_sigaction = SIG_DFL;
        }
    }
    return 0;
}
// 根据设计对信号进行相应处理
int do_signal(void) {
    struct proc *p = curr_proc();

    for (int signo = SIGMIN; signo <= SIGMAX; signo++) {
        if ((p->signal.sigpending & sigmask(signo)) && !(p->signal.sigmask & sigmask(signo))) {
            sigaction_t *sa = &p->signal.sa[signo];
            // SIGKILL 直接终止
            if (signo == SIGKILL) {
                setkilled(p, -10 - signo);
                sigdelset(&p->signal.sigpending, signo);
                return 0;
            }
            // 对于忽略信号，不进行处理
            if (sa->sa_sigaction == SIG_IGN) {
                sigdelset(&p->signal.sigpending, signo);
            } else if (sa->sa_sigaction == SIG_DFL) {
                setkilled(p, -10 - signo);
                sigdelset(&p->signal.sigpending, signo);
            } else {
                struct trapframe *tf = p->trapframe;
                // 计算新栈指针
                uint64 new_sp = tf->sp - sizeof(struct trapframe) - sizeof(sigset_t);
                // 将 trapframe 和 oldmask 分两次拷贝到用户栈
                sigset_t oldmask = p->signal.sigmask;
                acquire(&p->mm->lock);
                // 复制trapframe到用户栈
                copy_to_user(p->mm, new_sp, (char *)tf, sizeof(struct trapframe));
                // 复制oldmask到用户栈
                copy_to_user(p->mm, new_sp + sizeof(struct trapframe), (char *)&oldmask, sizeof(sigset_t));
                release(&p->mm->lock);
                // 处理信号
                p->signal.sigmask |= sa->sa_mask;
                sigaddset(&p->signal.sigmask, signo);
                sigdelset(&p->signal.sigpending, signo);
                // 设置trapframe跳转到handler
                tf->sp  = new_sp;  // 更新栈指针到备份数据顶部
                tf->epc = (uint64)sa->sa_sigaction;
                tf->a0  = signo;
                tf->ra  = (uint64)sa->sa_restorer;  // 返回地址
                break;
            }
        }
    }
    return 0;
}

// syscall handlers:
//  sys_* functions are called by syscall.c

int sys_sigaction(int signo, const sigaction_t __user *act, sigaction_t __user *oldact) {
    struct proc *p = curr_proc();
    acquire(&p->mm->lock);
    if (oldact) {
        copy_to_user(p->mm, (uint64)oldact, (char *)&p->signal.sa[signo], sizeof(sigaction_t));
    }
    if (act) {
        sigaction_t sa;
        copy_from_user(p->mm, (char *)&sa, (uint64)act, sizeof(sigaction_t));
        p->signal.sa[signo] = sa;
    }
    release(&p->mm->lock);
    return 0;
}

int sys_sigreturn() {
    struct proc *p       = curr_proc();
    struct trapframe *tf = p->trapframe;
    sigset_t oldmask;
    acquire(&p->mm->lock);
    // 从用户栈复制trapframe
    copy_from_user(p->mm, (char *)tf, tf->sp, sizeof(struct trapframe));
    // 从用户栈复制oldmask
    copy_from_user(p->mm, (char *)&oldmask, tf->sp + sizeof(struct trapframe), sizeof(sigset_t));
    release(&p->mm->lock);
    p->signal.sigmask = oldmask;
    return 0;
}

int sys_sigprocmask(int how, const sigset_t __user *set, sigset_t __user *oldset) {
    struct proc *p = curr_proc();
    acquire(&p->mm->lock);
    if (oldset) {
        copy_to_user(p->mm, (uint64)oldset, (char *)&p->signal.sigmask, sizeof(sigset_t));
    }
    if (set) {
        sigset_t newmask;
        copy_from_user(p->mm, (char *)&newmask, (uint64)set, sizeof(sigset_t));
        // SIGKILL不能被mask
        sigdelset(&newmask, SIGKILL);
        switch (how) {
            case SIG_BLOCK:
                p->signal.sigmask |= newmask;
                break;
            case SIG_UNBLOCK:
                p->signal.sigmask &= ~newmask;
                break;
            case SIG_SETMASK:
                p->signal.sigmask = newmask;
                break;
            default:
        }
    }
    release(&p->mm->lock);
    return 0;
}

int sys_sigpending(sigset_t __user *set) {
    struct proc *p = curr_proc();
    acquire(&p->mm->lock);
    copy_to_user(p->mm, (uint64)set, (char *)&p->signal.sigpending, sizeof(sigset_t));
    release(&p->mm->lock);
    return 0;
}

int sys_sigkill(int pid, int signo, int code) {
    struct proc *p;
    for (int i = 0; i < NPROC; i++) {
        p = pool[i];
        if (p->pid == pid && p->state != UNUSED) {
            sigaddset(&p->signal.sigpending, signo);
            return 0;
        }
    }
    return -1;
}