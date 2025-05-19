#include <defs.h>
#include <proc.h>
#include <trap.h>

#include "ksignal.h"
int siginit(struct proc *p) {
    for (int i = SIGMIN; i <= SIGMAX; i++) {
        p->signal.sa[i].sa_sigaction = SIG_DFL;  // 默认信号动作
        sigemptyset(&p->signal.sa[i].sa_mask);   // 清空信号掩码
        p->signal.sa[i].sa_restorer = NULL;      // 无恢复函数
    }
    sigemptyset(&p->signal.sigmask);     // 初始化进程信号掩码
    sigemptyset(&p->signal.sigpending);  // 清空挂起信号
    return 0;
}
int siginit_fork(struct proc *parent, struct proc *child) {
    memmove(&child->signal.sa[SIGMIN], &parent->signal.sa[SIGMIN],
            sizeof(struct sigaction) * (SIGMAX - SIGMIN + 1));  // 复制信号动作
    child->signal.sigmask = parent->signal.sigmask;             // 继承信号掩码
    sigemptyset(&child->signal.sigpending);                     // 清空挂起信号
    return 0;
}
int siginit_exec(struct proc *p) {
    for (int i = SIGMIN; i <= SIGMAX; i++) {
        if (p->signal.sa[i].sa_sigaction != SIG_IGN) {
            p->signal.sa[i].sa_sigaction = SIG_DFL;  // 重置为默认动作
        }
    }
    return 0;
}
int do_signal(void) {
    struct proc *p = curr_proc();
    for (int signo = SIGMIN; signo <= SIGMAX; signo++) {
        if (sigismember(&p->signal.sigpending, signo) && !sigismember(&p->signal.sigmask, signo)) {
            sigaction_t *sa = &p->signal.sa[signo];
            if (signo == SIGKILL) {
                setkilled(p, -10 - signo);  // 终止进程
                sigdelset(&p->signal.sigpending, signo);
                return 0;
            } else if (sa->sa_sigaction == SIG_IGN) {
                sigdelset(&p->signal.sigpending, signo);  // 忽略信号
            } else if (sa->sa_sigaction == SIG_DFL) {
                setkilled(p, -10 - signo);  // 默认终止
                sigdelset(&p->signal.sigpending, signo);
            } else {
                struct trapframe *tf = p->trapframe;
                // 分配用户栈空间并对齐到 16 字节
                uint64 new_sp    = PGROUNDDOWN(tf->sp - sizeof(struct trapframe) - sizeof(sigset_t));
                sigset_t oldmask = p->signal.sigmask;

                acquire(&p->mm->lock);
                copy_to_user(p->mm, new_sp, (char *)tf, sizeof(struct trapframe));                   // 备份 trapframe
                copy_to_user(p->mm, new_sp + sizeof(struct trapframe), &oldmask, sizeof(sigset_t));  // 备份掩码
                release(&p->mm->lock);

                // 更新信号掩码并处理信号
                p->signal.sigmask |= sa->sa_mask;
                sigaddset(&p->signal.sigmask, signo);
                sigdelset(&p->signal.sigpending, signo);

                tf->sp  = new_sp;
                tf->epc = (uint64)sa->sa_sigaction;  // 跳转到处理程序
                tf->a0  = signo;                     // 传递信号编号
                tf->ra  = (uint64)sa->sa_restorer;   // 设置返回地址
                break;
            }
        }
    }
    return 0;
}
int sys_sigaction(int signo, const sigaction_t __user *act, sigaction_t __user *oldact) {
    struct proc *p = curr_proc();
    if (signo < SIGMIN || signo > SIGMAX)
        return -1;  // 信号编号检查
    acquire(&p->mm->lock);
    if (oldact) {
        copy_to_user(p->mm, (uint64)oldact, &p->signal.sa[signo], sizeof(sigaction_t));
    }
    if (act) {
        copy_from_user(p->mm, &p->signal.sa[signo], (uint64)act, sizeof(sigaction_t));
    }
    release(&p->mm->lock);
    return 0;
}
int sys_sigreturn(void) {
    struct proc *p       = curr_proc();
    struct trapframe *tf = p->trapframe;
    sigset_t oldmask;
    acquire(&p->mm->lock);
    copy_from_user(p->mm, (char *)tf, tf->sp, sizeof(struct trapframe));                   // 恢复 trapframe
    copy_from_user(p->mm, &oldmask, tf->sp + sizeof(struct trapframe), sizeof(sigset_t));  // 恢复掩码
    release(&p->mm->lock);
    p->signal.sigmask = oldmask;
    return 0;
}
int sys_sigprocmask(int how, const sigset_t __user *set, sigset_t __user *oldset) {
    struct proc *p = curr_proc();
    acquire(&p->mm->lock);
    if (oldset) {
        copy_to_user(p->mm, (uint64)oldset, &p->signal.sigmask, sizeof(sigset_t));
    }
    if (set) {
        sigset_t newmask;
        copy_from_user(p->mm, &newmask, (uint64)set, sizeof(sigset_t));
        sigdelset(&newmask, SIGKILL);  // SIGKILL 不可屏蔽
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
                return -1;
        }
    }
    release(&p->mm->lock);
    return 0;
}
int sys_sigpending(sigset_t __user *set) {
    struct proc *p = curr_proc();
    acquire(&p->mm->lock);
    copy_to_user(p->mm, (uint64)set, &p->signal.sigpending, sizeof(sigset_t));
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